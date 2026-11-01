/*
 * cps3_hw.c  Board memory blocks and ROM loading
 *
 * Loads the ROM files cps3_romset.c makes from the ROM set: the BIOS, the graphics ROMs (user5) and the pattern
 * directory.
 * The directory is big-endian with program-ROM addresses, so it is converted to host records here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "structs.h"
#include "cps3_host.h"
#include "cps3_sound.h"

u16 cps3_sprite_ram[CPS3_SPRITE_RAM_SIZE / 2];
/* Colour RAM must be 256 KB aligned. The game computes the text palette bank from its address. */
static u16 color_ram_store[CPS3_COLOR_RAM_SIZE];
u16* cps3_color_ram;
u16 cps3_video_reg[CPS3_VIDEO_REG_SIZE / 2];
u16 cps3_sound_reg[CPS3_SOUND_REG_SIZE / 2];
u8 cps3_char_ram[CPS3_CHAR_RAM_SIZE];
u16 cps3_io_reg[CPS3_IO_REG_SIZE / 2];
u16 cps3_ss_ram[CPS3_SS_RAM_SIZE / 2];
u16 cps3_ss_reg[CPS3_SS_REG_SIZE / 2];
u16 cps3_sram[CPS3_SRAM_SIZE / 2];
u16 cps3_irq_ack[4];
u16 cps3_cd_reg[4];
u8 cps3_sh2_reg[0x200];

u32* cps3_user5_page[CPS3_USER5_PAGES];
u8 cps3_user5_ref[CPS3_USER5_PAGES];
u8 cps3_user5_ahead[CPS3_USER5_PAGES];

#define PAGE_BYTES (4u << CPS3_USER5_PAGE_SHIFT)
static FILE* user5_file;            /* streaming state */
static u32 user5_file_size;
static u32 user5_slots;
static u32** user5_slot_mem;
static u32* user5_slot_page;
static u32 user5_hand;
static u32 user5_reserve;
static const Cps3User5Reader* user5_reader;
static char user5_path[512];

void cps3_user5_stream(u32 cached_pages, u32 reserve_bytes) {
    user5_slots = cached_pages < 2 ? 2 : cached_pages;
    user5_reserve = reserve_bytes;
}

u32 cps3_user5_cached_pages(void) {
    return user5_slots;
}

/* Picks a cache slot with the clock algorithm, so pages in use stay loaded.
   An SD card read costs a 3DS about half a frame. */
static u32* claim_slot(u32 page) {
    u32 slot;
    for (;;) {
        u32 old;
        slot = user5_hand;
        user5_hand = (user5_hand + 1) % user5_slots;
        old = user5_slot_page[slot];
        if (old == ~0u || !cps3_user5_ref[old]) {
            break;
        }
        cps3_user5_ref[old] = 0;
    }
    if (user5_slot_page[slot] != ~0u) {
        cps3_user5_page[user5_slot_page[slot]] = NULL;
        cps3_user5_ahead[user5_slot_page[slot]] = 0;
    }
    user5_slot_page[slot] = page;
    return user5_slot_mem[slot];
}

u32* cps3_user5_fault(u32 page) {
    static u32 zero_page[1u << CPS3_USER5_PAGE_SHIFT];
    u32 ofs = page * PAGE_BYTES;
    u32* mem;
    if (user5_file == NULL || ofs >= user5_file_size) {
        cps3_user5_page[page] = zero_page;
        return zero_page;
    }
    mem = claim_slot(page);
    if (user5_reader != NULL && user5_reader->take(page, mem)) {
        /* read ahead */
    } else if (user5_reader != NULL && user5_reader->read != NULL && user5_reader->read(page, mem)) {
        /* read by the reader */
    } else {
        u32 want = ofs + PAGE_BYTES <= user5_file_size ? PAGE_BYTES : user5_file_size - ofs;
        size_t got;
        memset(mem, 0, PAGE_BYTES);
        fseek(user5_file, ofs, SEEK_SET);
        got = fread(mem, 1, want, user5_file);
        (void)got;
    }
    cps3_user5_page[page] = mem;
    if (user5_reader != NULL) {
        user5_reader->ahead(page);
    }
    return mem;
}

/* Adds a read-ahead page. It is not marked used, so it is evicted first if unused. */
void cps3_user5_install(u32 page, const void* data) {
    u32* mem;
    if (page >= CPS3_USER5_PAGES || cps3_user5_page[page] != NULL || user5_slots == 0) {
        return;
    }
    mem = claim_slot(page);
    memcpy(mem, data, PAGE_BYTES);
    cps3_user5_ref[page] = 0;
    cps3_user5_ahead[page] = 1;
    cps3_user5_page[page] = mem;
}

/* A read-ahead page got used, so read further ahead. */
void cps3_user5_ahead_used(u32 page) {
    cps3_user5_ahead[page] = 0;
    if (user5_reader != NULL) {
        user5_reader->ahead(page);
    }
}

void cps3_user5_set_reader(const Cps3User5Reader* r) {
    user5_reader = r;
}

int cps3_user5_cached(u32 page) {
    return page >= CPS3_USER5_PAGES || cps3_user5_page[page] != NULL;
}

/* Returns 1 if the page is loaded. Otherwise asks the reader for it and returns 0. */
int cps3_user5_ready(u32 page) {
    if (page >= CPS3_USER5_PAGES) {
        return 1;
    }
    if (cps3_user5_page[page] == NULL) {
        if (user5_reader != NULL && user5_reader->want != NULL) {
            user5_reader->want(page);
            return 0;
        }
        cps3_user5_fault(page);
    }
    cps3_user5_ref[page] = 1;
    if (cps3_user5_ahead[page]) {
        cps3_user5_ahead[page] = 0;
    }
    return 1;
}

/* Asks the reader for a page that will be needed soon. */
void cps3_user5_soon(u32 page) {
    if (page < CPS3_USER5_PAGES && cps3_user5_page[page] == NULL && user5_reader != NULL &&
        user5_reader->want != NULL) {
        user5_reader->want(page);
    }
}

const char* cps3_user5_path(void) {
    return user5_file != NULL ? user5_path : NULL;
}

u32 cps3_user5_file_size(void) {
    return user5_file_size;
}

static s32 open_user5_stream(const char* dir) {
    char path[512];
    u32 i;
    void* hold;
    snprintf(path, sizeof(path), "%s/%s", dir, "user5.bin");
    snprintf(user5_path, sizeof(user5_path), "%s", path);
    user5_file = fopen(path, "rb");
    if (user5_file == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    fseek(user5_file, 0, SEEK_END);
    user5_file_size = (u32)ftell(user5_file);
    user5_slot_mem = calloc(user5_slots, sizeof(u32*));
    user5_slot_page = malloc(user5_slots * sizeof(u32));
    if (user5_slot_mem == NULL || user5_slot_page == NULL) {
        fprintf(stderr, "out of memory for the graphics ROM cache\n");
        return -1;
    }
    /* Fill the cache with as many pages as fit, keeping the reserve free (task stacks, sound and so on).
       The reserve is held while the pages are allocated, so it is kept even when every page fits. */
    hold = user5_reserve != 0 ? malloc(user5_reserve) : NULL;
    for (i = 0; i < user5_slots; i++) {
        user5_slot_page[i] = ~0u;
        if ((user5_slot_mem[i] = malloc(PAGE_BYTES)) == NULL) {
            break;
        }
    }
    if (hold == NULL && user5_reserve != 0) {
        /* not even the reserve fitted: give pages back for it */
        u32 give = (user5_reserve + PAGE_BYTES - 1) / PAGE_BYTES;
        u32 n = i > give ? i - give : 0;
        while (i > n) {
            free(user5_slot_mem[--i]);
        }
    }
    user5_slots = i;
    free(hold);
    if (user5_slots < 2) {
        fprintf(stderr, "out of memory for the graphics ROM cache\n");
        return -1;
    }
    return 0;
}

u8 cps3_bios[CPS3_BIOS_SIZE];

/* The pattern directory (cg_data_tbl), at 06800000 on the board. */
#define CG_BASE 0x06800000
#define CG_ENTRIES 0x10000
CharGfxEntry* host_cg_data_tbl;

/* Section bounds reset_entry uses. The C runtime already did that work. */
int *B_BGN, *B_END, *D_BGN, *D_END, *D_ROM;

/* fixed_addr.src's absolute symbols, as offsets into the host's blocks. */
#if defined(_WIN32) && !defined(_WIN64)
#define ASM_SYM(name) "_" #name
#else
#define ASM_SYM(name) #name
#endif
#define FIXED_SYM(name, block, ofs) \
    __asm__(".globl " ASM_SYM(name) "\n.set " ASM_SYM(name) ", " ASM_SYM(block) " + " #ofs)
FIXED_SYM(bios_region_code, cps3_bios, 0x1FECB);
FIXED_SYM(bios_cd_flags, cps3_bios, 0x1FECC);
FIXED_SYM(bios_area_code, cps3_bios, 0x1FEDB);
FIXED_SYM(snd_key_on_reg, cps3_sound_reg, 0x200);

u8 cps3_low_read8(u32 addr, const char* site) {
    (void)site;
    return cps3_bios[addr & (CPS3_BIOS_SIZE - 1)];
}

u32 cps3_low_read_be(u32 addr, u32 size, const char* site) {
    u32 v = 0;
    u32 i;
    (void)site;
    for (i = 0; i < size && i < 4; i++) {
        v = (v << 8) | cps3_bios[(addr + i) & (CPS3_BIOS_SIZE - 1)];
    }
    return v;
}

void cps3_low_read16(void* dst, u32 addr, u32 bytes, const char* site) {
    u16* d = dst;
    u32 i;
    (void)site;
    for (i = 0; i + 1 < bytes; i += 2) {
        u32 a = (addr + i) & (CPS3_BIOS_SIZE - 1);
        d[i / 2] = (u16)((cps3_bios[a] << 8) | cps3_bios[a + 1]);
    }
}

/* Only the test menu's ROM check reads this. */
u8* cps3_simm_window(void) {
    static u8 window[0x200000];
    return window;
}

static u8* read_file(const char* dir, const char* name, u32* size) {
    char path[512];
    FILE* f;
    u8* data;
    long n;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(n);
    if (data == NULL || fread(data, 1, n, f) != (size_t)n) {
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    *size = (u32)n;
    return data;
}

static u16 be16(const u8* p) {
    return (u16)((p[0] << 8) | p[1]);
}

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Converts one big-endian set. Some sets have an extra unused cell. */
static CharGfxSet* convert_set(const u8* rom, u32 rom_size, u32 addr) {
    const u8* p;
    CharGfxSet* set;
    CHAR_CELL* cell;
    u32 ofs = addr - CG_BASE;
    u16 count;
    u16 cells;
    u32 i;
    size_t bytes;
    if (ofs + 12 > rom_size) {
        return NULL;
    }
    p = rom + ofs;
    count = be16(p + 4);
    cells = be16(p + 6);
    bytes = offsetof(CharGfxSet, chunk) + (count + 1) * sizeof(CharGfxChunk) + (cells + 1) * sizeof(CHAR_CELL);
    set = calloc(1, bytes);
    if (set == NULL) {
        return NULL;
    }
    set->slot = be16(p + 0);
    set->size = be16(p + 2);
    set->count = count;
    set->cells = cells;
    set->prep = be32(p + 8);
    p += 12;
    for (i = 0; i < count; i++, p += 8) {
        set->chunk[i].src = be32(p);
        set->chunk[i].size = be16(p + 4);
        set->chunk[i].dst = be16(p + 6);
    }
    /* cells follow the chunks */
    cell = (CHAR_CELL*)&set->chunk[count];
    for (i = 0; i < cells + 1u && (u32)(p - rom) + 8 <= rom_size; i++, p += 8) {
        cell[i].code = (s16)be16(p);
        cell[i].col = (s16)be16(p + 2);
        cell[i].x = (s16)be16(p + 4);
        cell[i].y = (s16)be16(p + 6);
    }
    return set;
}

/* builds the pattern directory from cg.bin's contents; frees rom */
static s32 load_cg(u8* rom, u32 size) {
    CharGfxSet** done;
    s32 i;
    if (rom == NULL) {
        return -1;
    }
    host_cg_data_tbl = calloc(CG_ENTRIES, sizeof(CharGfxEntry));
    done = calloc(size / 4, sizeof(CharGfxSet*));
    if (host_cg_data_tbl == NULL || done == NULL) {
        fprintf(stderr, "out of memory for the pattern directory\n");
        free(done);
        free(rom);
        return -1;
    }
    for (i = 0; i < CG_ENTRIES && (u32)i * 8 + 8 <= size; i++) {
        const u8* e = rom + i * 8;
        u32 addr = be32(e + 4);
        host_cg_data_tbl[i].x = (s16)be16(e);
        host_cg_data_tbl[i].y = (s16)be16(e + 2);
        if (addr >= CG_BASE && addr - CG_BASE < size) {
            u32 k = (addr - CG_BASE) / 4;
            if (done[k] == NULL && (done[k] = convert_set(rom, size, addr)) == NULL && addr - CG_BASE + 12 <= size) {
                fprintf(stderr, "out of memory for the pattern directory\n");
                free(done);
                free(rom);
                return -1;
            }
            host_cg_data_tbl[i].set = done[k];
        }
    }
    free(done);
    free(rom);
    return 0;
}

void cps3_hw_init(void) {
    uintptr_t a = ((uintptr_t)color_ram_store + CPS3_COLOR_RAM_SIZE - 1) & ~(uintptr_t)(CPS3_COLOR_RAM_SIZE - 1);
    cps3_color_ram = (u16*)a;
    /* All switches released. IO_REG + 0xA00 reads all ones on a production board. */
    memset(cps3_io_reg, 0xFF, 16);
    memset(&cps3_io_reg[0xA00 / 2], 0xFF, 32);
    cps3_sound_reset();
}

/* Opposite directions cancel out (a keyboard can press both). */
static u32 clean_lever(u32 in) {
    if ((in & (CPS3_IN_LEFT | CPS3_IN_RIGHT)) == (CPS3_IN_LEFT | CPS3_IN_RIGHT)) {
        in &= ~(u32)(CPS3_IN_LEFT | CPS3_IN_RIGHT);
    }
    if ((in & (CPS3_IN_UP | CPS3_IN_DOWN)) == (CPS3_IN_UP | CPS3_IN_DOWN)) {
        in &= ~(u32)(CPS3_IN_UP | CPS3_IN_DOWN);
    }
    return in;
}

/* Packs inputs into the six-button switch ports (active low).
   IO_REG + 0: system, coins, starts and P2 heavy kick. + 2: levers and punches. + 4: the other kicks. */
void cps3_set_inputs(u32 p1, u32 p2, u32 sys) {
    u32 system = sys & (CPS3_SYS_SERVICE | CPS3_SYS_TEST);
    u32 players;
    u32 kicks = 0;
    p1 = clean_lever(p1);
    p2 = clean_lever(p2);
    players = (p1 & 0x7F) | ((p2 & 0x7F) << 8);
    if (p1 & CPS3_IN_COIN) {
        system |= 0x100;
    }
    if (p2 & CPS3_IN_COIN) {
        system |= 0x200;
    }
    if (p1 & CPS3_IN_START) {
        system |= 0x1000;
    }
    if (p2 & CPS3_IN_START) {
        system |= 0x2000;
    }
    if (p2 & CPS3_IN_HK) {
        system |= 0x400;
    }
    if (p1 & CPS3_IN_LK) {
        kicks |= 0x08;
    }
    if (p1 & CPS3_IN_MK) {
        kicks |= 0x04;
    }
    if (p1 & CPS3_IN_HK) {
        kicks |= 0x02;
    }
    if (p2 & CPS3_IN_LK) {
        kicks |= 0x10;
    }
    if (p2 & CPS3_IN_MK) {
        kicks |= 0x20;
    }
    cps3_io_reg[0] = (u16)~system;
    cps3_io_reg[1] = (u16)~players;
    cps3_io_reg[2] = (u16)~kicks;
}

s32 cps3_load_rom(const char* dir) {
    u32 size;
    u8* data;
    cps3_hw_init();
    data = read_file(dir, "bios.bin", &size);
    if (data == NULL) {
        return -1;
    }
    memcpy(cps3_bios, data, size < CPS3_BIOS_SIZE ? size : CPS3_BIOS_SIZE);
    free(data);
    /* Load the directory first. It briefly needs a lot of memory that the ROM cache would take. */
    data = read_file(dir, "cg.bin", &size);
    if (load_cg(data, size) != 0) {
        return -1;
    }

    if (user5_slots != 0) {
        if (open_user5_stream(dir) != 0) {
            return -1;
        }
    } else {
        u32* all;
        u32 i;
        data = read_file(dir, "user5.bin", &size);
        if (data == NULL) {
            return -1;
        }
        all = calloc(CPS3_USER5_SIZE / 4, 4);
        memcpy(all, data, size < CPS3_USER5_SIZE ? size : CPS3_USER5_SIZE);
        free(data);
        for (i = 0; i < CPS3_USER5_PAGES; i++) {
            cps3_user5_page[i] = all + (i << CPS3_USER5_PAGE_SHIFT);
        }
    }
    return 0;
}

s32 cps3_load_rom_data(const u8* bios, u8* cg, u32 cg_size, u32* user5) {
    u32 i;
    cps3_hw_init();
    memcpy(cps3_bios, bios, CPS3_BIOS_SIZE);
    if (load_cg(cg, cg_size) != 0) {
        free(user5);
        return -1;
    }
    for (i = 0; i < CPS3_USER5_PAGES; i++) {
        cps3_user5_page[i] = user5 + (i << CPS3_USER5_PAGE_SHIFT);
    }
    return 0;
}

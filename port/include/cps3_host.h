/*
 * cps3_host.h  CPS3 board memory and devices for the host build
 *
 * Board blocks are host u16 arrays, since the game writes them 16 bits at a time.
 * A 32-bit board word is high half first (see cps3_be32).
 */

#ifndef CPS3_HOST_H
#define CPS3_HOST_H

#include "types.h"

#define CPS3_SPRITE_RAM_SIZE    0x80000
#define CPS3_COLOR_RAM_SIZE     0x40000     /* 0x20000 colours */
#define CPS3_VIDEO_REG_SIZE     0x10000
#define CPS3_SOUND_REG_SIZE     0x1000
#define CPS3_CHAR_RAM_SIZE      0x800000    /* 8 banks of 1 MB */
#define CPS3_IO_REG_SIZE        0x2000
#define CPS3_SS_RAM_SIZE        0x10000
#define CPS3_SS_REG_SIZE        0x100
#define CPS3_SRAM_SIZE          0x800
#define CPS3_USER5_SIZE         0x5000000   /* graphics and sample ROMs, simm3-6 */
#define CPS3_BIOS_SIZE          0x80000

extern u16 cps3_sprite_ram[CPS3_SPRITE_RAM_SIZE / 2];
extern u16* cps3_color_ram;                  /* aligned to its size, as on the board */
extern u16 cps3_video_reg[CPS3_VIDEO_REG_SIZE / 2];
extern u16 cps3_sound_reg[CPS3_SOUND_REG_SIZE / 2];
extern u8 cps3_char_ram[CPS3_CHAR_RAM_SIZE];
extern u16 cps3_io_reg[CPS3_IO_REG_SIZE / 2];
extern u16 cps3_ss_ram[CPS3_SS_RAM_SIZE / 2];
extern u16 cps3_ss_reg[CPS3_SS_REG_SIZE / 2];
extern u16 cps3_sram[CPS3_SRAM_SIZE / 2];
extern u16 cps3_irq_ack[4];
extern u16 cps3_cd_reg[4];
extern u8 cps3_sh2_reg[0x200];
u8* cps3_simm_window(void);

/* Graphics ROM (user5), as MAME stores it: one host u32 per 4 bytes, in 16 KB pages.
   With streaming (3DS), only some pages are kept in memory and the rest are read on demand. */
#define CPS3_USER5_PAGE_SHIFT   12                              /* in u32s: 16 KB */
#define CPS3_USER5_PAGES        (CPS3_USER5_SIZE >> (CPS3_USER5_PAGE_SHIFT + 2))
extern u32* cps3_user5_page[CPS3_USER5_PAGES];
extern u8 cps3_user5_ref[CPS3_USER5_PAGES];     /* page used since the cache last checked */
u32* cps3_user5_fault(u32 page);
/* Call before cps3_load_rom to enable streaming. Keeps reserve_bytes free if memory is short. */
void cps3_user5_stream(u32 cached_pages, u32 reserve_bytes);
u32 cps3_user5_cached_pages(void);          /* pages the cache holds */
/* Hooks for a background read-ahead thread (3DS).
   take: copy a page into dst if the reader has it (returns 1), else 0.
   ahead: a page was missed or first used, so queue the pages after it. */
typedef struct {
    int (*take)(u32 page, void* dst);
    void (*ahead)(u32 page);
    void (*want)(u32 page);     /* urgent: fetch this page first */
} Cps3User5Reader;
void cps3_user5_set_reader(const Cps3User5Reader* r);
int cps3_user5_cached(u32 page);
/* For callers that must not block (sound): 1 if loaded, else asks the reader and returns 0. */
int cps3_user5_ready(u32 page);
void cps3_user5_soon(u32 page);             /* ask the reader for it now */
void cps3_user5_install(u32 page, const void* data);   /* call from the game thread */
extern u8 cps3_user5_ahead[CPS3_USER5_PAGES];   /* read ahead, not used yet */
void cps3_user5_ahead_used(u32 page);
const char* cps3_user5_path(void);          /* the streamed file, or NULL */
u32 cps3_user5_file_size(void);
extern u8 cps3_bios[CPS3_BIOS_SIZE];
void cps3_hw_init(void);
s32 cps3_load_rom(const char* dir);     /* calls cps3_hw_init */
/* The same from memory (the PC loads the ROM set's zip): the BIOS (CPS3_BIOS_SIZE), cg.bin's contents and the
   graphics ROM (CPS3_USER5_SIZE). Takes cg and user5, which must come from malloc. No streaming. */
s32 cps3_load_rom_data(const u8* bios, u8* cg, u32 cg_size, u32* user5);

/* Input bits, matching the game's LEVER_* and BUTTON_* bits (work.h). */
#define CPS3_IN_UP      0x0001
#define CPS3_IN_DOWN    0x0002
#define CPS3_IN_LEFT    0x0004
#define CPS3_IN_RIGHT   0x0008
#define CPS3_IN_LP      0x0010
#define CPS3_IN_MP      0x0020
#define CPS3_IN_HP      0x0040
#define CPS3_IN_LK      0x0080
#define CPS3_IN_MK      0x0100
#define CPS3_IN_HK      0x0200
#define CPS3_IN_COIN    0x0800
#define CPS3_IN_START   0x1000
#define CPS3_SYS_SERVICE 1                  /* service coin */
#define CPS3_SYS_TEST   2                   /* test menu */
void cps3_set_inputs(u32 p1, u32 p2, u32 sys);

/* Reads through null pointers. Board address 0 is the BIOS, and the game relies on reading it.
   cps3_low_read16 copies big-endian 16-bit words into host u16s. */
void cps3_low_read16(void* dst, u32 addr, u32 bytes, const char* site);
u8 cps3_low_read8(u32 addr, const char* site);

/* the board's 32-bit word at a byte offset of a u16 block */
#define cps3_be32(w, ofs) (((u32)(w)[(ofs) >> 1] << 16) | (w)[((ofs) >> 1) + 1])

/* graphics ROM accessors */
static inline u32 cps3_user5_dword(u32 i) {
    u32 page = i >> CPS3_USER5_PAGE_SHIFT;
    u32* p;
    if (page >= CPS3_USER5_PAGES) {
        return 0;
    }
    p = cps3_user5_page[page];
    if (p == 0) {
        p = cps3_user5_fault(page);
    }
    cps3_user5_ref[page] = 1;
    return p[i & ((1u << CPS3_USER5_PAGE_SHIFT) - 1)];
}
static inline u8 cps3_user5_byte(u32 a) {
    a ^= 1;
    return cps3_user5_dword(a >> 2) >> ((a & 3) * 8);
}
static inline u16 cps3_user5_word(u32 w) {
    u32 d = cps3_user5_dword(w >> 1);
    return (w & 1) ? (u16)d : (u16)(d >> 16);
}

/* character RAM, in the board's byte order */
static inline u8 cps3_char_byte(u32 a) {
    return cps3_char_ram[a & (CPS3_CHAR_RAM_SIZE - 1)];
}
/* A bit per 16x16 tile written by DMA. A GPU renderer clears it on upload. */
extern u8 cps3_char_dirty[CPS3_CHAR_RAM_SIZE / 256 / 8];
/* Deferred 6-bit character DMA. cps3_chr_dma_work runs queued jobs when there is time.
   Before showing a tile marked in cps3_char_pending, call cps3_chr_dma_need. */
extern int cps3_chr_dma_defer;
extern u32 cps3_chr_jobs;
extern u8 cps3_char_pending[CPS3_CHAR_RAM_SIZE / 256 / 8];
static inline int cps3_char_tile_pending(u32 tile) {
    return cps3_chr_jobs > 0 && ((cps3_char_pending[tile >> 3] >> (tile & 7)) & 1);
}
void cps3_chr_dma_work(u32 bytes);
void cps3_chr_dma_need(u32 tile);
void cps3_chr_dma_flush(void);

/* video DMA (cps3_video_dma.c) */
extern u16 cps3_dma_status;                 /* DMA busy bits, always idle */
void cps3_chr_dma_start(void);
void cps3_col_dma_start(void);
void cps3_sprite_dma(void);
extern u32 cps3_spritelist[CPS3_SPRITE_RAM_SIZE / 4];
extern u32 cps3_gscroll_buff[8];

/* EEPROM (cps3_eeprom.c) */
void cps3_eeprom_load(const char* path);
s32 cps3_eeprom_read(s16 n, volatile u16* rom, u16* buf);
s32 cps3_eeprom_write(s16 n, volatile u16* rom, u16* buf);

/* interrupts (cps3_kernel.c) */
void cps3_dma_end_irq(void);

#endif

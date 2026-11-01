/*
 * cps3_romset.c  Reads MAME's ROM set zip: the PC loads it at start-up, the 3DS unpacks it to files once
 *
 * Decrypts the BIOS and the program SIMM that holds the graphics pattern directory (cg.bin), and interleaves the
 * graphics SIMMs into the graphics ROM (user5.bin).
 *
 * Takes a non-merged sfiii3nr1.zip, or a split set (sfiii3nr1.zip next to its parent sfiii3.zip), or a merged
 * sfiii3.zip that keeps sfiii3nr1's own files in a sfiii3nr1 folder.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3_romset.h"

#define CHIP        0x200000            /* every SIMM chip is 2 MB */
#define CG_BASE     0x06800000          /* the program SIMM 2's address */
#define CG_SIZE     (0x06E5E920 - CG_BASE)
#define CLONE       "sfiii3nr1"
#define BIOS        "sfiii3_euro.29f400.u2"

char cps3_romset_error[512];

typedef struct {
    char name[128];
    u32 method, csize, usize, crc, offset;
} Entry;

typedef struct {
    FILE* f;
    Entry* e;
    u32 n;
    int merged;                         /* the parent's zip: look in the clone's folder first */
} Zip;

static Zip zips[2];

#define STEPS       37                  /* files read: the BIOS, 4 program chips, 32 graphics chips */
static void (*progress_fn)(u32 done, u32 total);
static u32 progress_done;

static u32 le16(const u8* p) {
    return p[0] | (p[1] << 8);
}

static u32 le32(const u8* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static int zip_open(Zip* z, const char* path) {
    u8* tail;
    u8* cd;
    long size, len, i;
    u32 count, cd_size, cd_off, k, pos;
    memset(z, 0, sizeof(*z));
    if ((z->f = fopen(path, "rb")) == NULL) {
        return 0;
    }
    fseek(z->f, 0, SEEK_END);
    size = ftell(z->f);
    len = size < 65557 ? size : 65557;  /* the end record and the longest comment */
    tail = malloc(len);
    fseek(z->f, size - len, SEEK_SET);
    if (tail == NULL || fread(tail, 1, len, z->f) != (size_t)len) {
        free(tail);
        return 0;
    }
    for (i = len - 22; i >= 0 && le32(tail + i) != 0x06054B50; i--) {
    }
    if (i < 0) {
        free(tail);
        return 0;
    }
    count = le16(tail + i + 10);
    cd_size = le32(tail + i + 12);
    cd_off = le32(tail + i + 16);
    free(tail);
    cd = malloc(cd_size);
    z->e = calloc(count, sizeof(Entry));
    fseek(z->f, cd_off, SEEK_SET);
    if (cd == NULL || z->e == NULL || fread(cd, 1, cd_size, z->f) != cd_size) {
        free(cd);
        return 0;
    }
    for (k = 0, pos = 0; k < count && pos + 46 <= cd_size && le32(cd + pos) == 0x02014B50; k++) {
        Entry* e = &z->e[z->n++];
        u32 nlen = le16(cd + pos + 28);
        e->method = le16(cd + pos + 10);
        e->crc = le32(cd + pos + 16);
        e->csize = le32(cd + pos + 20);
        e->usize = le32(cd + pos + 24);
        e->offset = le32(cd + pos + 42);
        snprintf(e->name, sizeof(e->name), "%.*s", (int)nlen, (const char*)cd + pos + 46);
        for (i = 0; e->name[i] != '\0'; i++) {
            e->name[i] = e->name[i] == '\\' ? '/' : e->name[i];
        }
        pos += 46 + nlen + le16(cd + pos + 30) + le16(cd + pos + 32);
    }
    free(cd);
    return 1;
}

static void zip_close(Zip* z) {
    if (z->f != NULL) {
        fclose(z->f);
    }
    free(z->e);
    memset(z, 0, sizeof(*z));
}

/* the name ends with suffix ("sfiii3-simm3.0", or ".29f400.u2" for the BIOS) */
static int name_is(const char* name, const char* suffix, int folder) {
    size_t n = strlen(name), m = strlen(suffix);
    const char* base = strrchr(name, '/');
    if (folder) {
        return base != NULL && (size_t)(base - name) == strlen(CLONE) && strncasecmp(name, CLONE, strlen(CLONE)) == 0 &&
               n >= m && strcasecmp(name + n - m, suffix) == 0;
    }
    return base == NULL && n >= m && strcasecmp(name + n - m, suffix) == 0;
}

static const Entry* find(Zip** in, const char* suffix) {
    int zi, folder;
    u32 k;
    for (zi = 0; zi < 2; zi++) {
        for (folder = zips[zi].merged; folder >= 0; folder--) {
            for (k = 0; k < zips[zi].n; k++) {
                if (name_is(zips[zi].e[k].name, suffix, folder)) {
                    *in = &zips[zi];
                    return &zips[zi].e[k];
                }
            }
        }
    }
    return NULL;
}

/* one file of the set, size bytes, checked against its CRC */
static u8* read_member(const char* suffix, u32 size) {
    Zip* z;
    const Entry* e = find(&z, suffix);
    u8 head[30];
    u8* comp;
    u8* out;
    if (e == NULL) {
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "%s is missing from the ROM set.", suffix);
        return NULL;
    }
    if (e->usize != size || (e->method != 0 && e->method != 8)) {
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "%s in the ROM set is not the expected file.", e->name);
        return NULL;
    }
    fseek(z->f, e->offset, SEEK_SET);
    if (fread(head, 1, 30, z->f) != 30 || le32(head) != 0x04034B50) {
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "The ROM set zip is damaged.");
        return NULL;
    }
    fseek(z->f, e->offset + 30 + le16(head + 26) + le16(head + 28), SEEK_SET);
    comp = malloc(e->csize ? e->csize : 1);
    out = malloc(size);
    if (comp == NULL || out == NULL || fread(comp, 1, e->csize, z->f) != e->csize) {
        free(comp);
        free(out);
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "Could not read %s from the ROM set.", e->name);
        return NULL;
    }
    if (e->method == 0) {
        memcpy(out, comp, size);
    } else {
        z_stream s;
        int ok;
        memset(&s, 0, sizeof(s));
        inflateInit2(&s, -MAX_WBITS);
        s.next_in = comp;
        s.avail_in = e->csize;
        s.next_out = out;
        s.avail_out = size;
        ok = inflate(&s, Z_FINISH) == Z_STREAM_END && s.total_out == size;
        inflateEnd(&s);
        if (!ok) {
            free(comp);
            free(out);
            snprintf(cps3_romset_error, sizeof(cps3_romset_error), "%s in the ROM set is damaged.", e->name);
            return NULL;
        }
    }
    free(comp);
    if (crc32(0, out, size) != e->crc) {
        free(out);
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "%s in the ROM set is damaged (CRC).", e->name);
        return NULL;
    }
    if (progress_fn != NULL) {
        progress_fn(++progress_done, STEPS);
    }
    return out;
}

/* the CPS3 address mask (MAME's cps3_state::cps3_mask, 3rd Strike's keys) */
static u16 rol16(u16 v, int n) {
    return (u16)((v << n) | (v >> (16 - n)));
}

static u16 rotxor(u16 val, u16 x) {
    u16 res = (u16)(val + rol16(val, 2));
    return (u16)(rol16(res, 4) ^ (res & (val ^ x)));
}

static u32 mask(u32 addr) {
    const u32 key1 = 0xA55432B4, key2 = 0x0C129981;
    u16 v;
    addr ^= key1;
    v = (u16)((addr & 0xFFFF) ^ 0xFFFF);
    v = rotxor(v, (u16)(key2 & 0xFFFF));
    v ^= (u16)(((addr >> 16) & 0xFFFF) ^ 0xFFFF);
    v = rotxor(v, (u16)(key2 >> 16));
    v ^= (u16)((addr & 0xFFFF) ^ (key2 & 0xFFFF));
    return v | ((u32)v << 16);
}

static void put_be32(u8* p, u32 w) {
    p[0] = (u8)(w >> 24);
    p[1] = (u8)(w >> 16);
    p[2] = (u8)(w >> 8);
    p[3] = (u8)w;
}

/* where the graphics ROM goes, 4 MB (a chip pair) at a time */
typedef int (*User5Put)(void* ctx, const u32* words, u32 count);

static int build(u8* bios, u8** cg_out, User5Put put, void* ctx) {
    Zip* z;
    u8* chip[4] = { NULL, NULL, NULL, NULL };
    u8* cg;
    u32* words;
    u8* raw;
    char name[32];
    u32 i;
    int simm, pair, c;

    /* BIOS: one 512 KB chip, encrypted. The set has two; the port uses the European one. */
    if ((raw = read_member(find(&z, BIOS) != NULL ? BIOS : ".29f400.u2", CPS3_BIOS_SIZE)) == NULL) {
        return 0;
    }
    for (i = 0; i < CPS3_BIOS_SIZE; i += 4) {
        put_be32(bios + i, ((u32)raw[i] << 24 | raw[i + 1] << 16 | raw[i + 2] << 8 | raw[i + 3]) ^ mask(i));
    }
    free(raw);

    /* program SIMM 2, a byte lane per chip, encrypted: its start is the pattern directory */
    cg = malloc(CG_SIZE);
    for (c = 0; c < 4 && cg != NULL; c++) {
        snprintf(name, sizeof(name), "sfiii3-simm2.%d", c);
        if ((chip[c] = read_member(name, CHIP)) == NULL) {
            break;
        }
    }
    if (cg == NULL || c < 4) {
        if (cg == NULL) {
            snprintf(cps3_romset_error, sizeof(cps3_romset_error), "Out of memory.");
        }
        for (c = 0; c < 4; c++) {
            free(chip[c]);
        }
        free(cg);
        return 0;
    }
    for (i = 0; i < CG_SIZE / 4; i++) {
        u32 w = (u32)chip[0][i] << 24 | chip[1][i] << 16 | chip[2][i] << 8 | chip[3][i];
        put_be32(cg + i * 4, w ^ mask(CG_BASE + i * 4));
    }
    for (c = 0; c < 4; c++) {
        free(chip[c]);
    }

    /* graphics SIMMs 3-6: chip pairs interleaved by halfword */
    words = malloc(CHIP * 2);
    if (words == NULL) {
        free(cg);
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "Out of memory.");
        return 0;
    }
    for (simm = 3; simm <= 6; simm++) {
        for (pair = 0; pair < 8; pair += 2) {
            u8* f0;
            u8* f1;
            u32 j;
            snprintf(name, sizeof(name), "sfiii3-simm%d.%d", simm, pair);
            f0 = read_member(name, CHIP);
            snprintf(name, sizeof(name), "sfiii3-simm%d.%d", simm, pair + 1);
            f1 = f0 != NULL ? read_member(name, CHIP) : NULL;
            if (f1 == NULL) {
                free(f0);
                free(words);
                free(cg);
                return 0;
            }
            for (j = 0; j < CHIP; j += 2) {
                words[j / 2] = (u32)f0[j] << 8 | (u32)f0[j + 1] << 24 | f1[j] | (u32)f1[j + 1] << 16;
            }
            free(f0);
            free(f1);
            if (!put(ctx, words, CHIP / 2)) {
                free(words);
                free(cg);
                return 0;
            }
        }
    }
    free(words);
    *cg_out = cg;
    return 1;
}

/* opens the set: the zip, and for a split set its parent next to it */
static int open_set(const char* zip_path) {
    char parent[512];
    const char* slash;
    cps3_romset_error[0] = '\0';
    progress_done = 0;
    if (!zip_open(&zips[0], zip_path)) {
        zip_close(&zips[0]);
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "Could not read %s as a zip.", zip_path);
        return 0;
    }
    slash = strrchr(zip_path, '/');
    if (strrchr(zip_path, '\\') > slash) {
        slash = strrchr(zip_path, '\\');
    }
    snprintf(parent, sizeof(parent), "%.*ssfiii3.zip", slash != NULL ? (int)(slash - zip_path + 1) : 0, zip_path);
    if (strcasecmp(parent, zip_path) == 0) {
        zips[0].merged = 1;             /* this is the parent: a merged set */
    } else if (!zip_open(&zips[1], parent)) {
        zip_close(&zips[1]);
    }
    return 1;
}

static void close_set(void) {
    zip_close(&zips[0]);
    zip_close(&zips[1]);
    progress_fn = NULL;
}

static int put_memory(void* ctx, const u32* words, u32 count) {
    u32** at = ctx;
    memcpy(*at, words, count * 4);
    *at += count;
    return 1;
}

s32 cps3_load_romset(const char* zip_path) {
    static u8 bios[CPS3_BIOS_SIZE];
    u8* cg = NULL;
    u32* user5;
    u32* at;
    int ok;
    if (!open_set(zip_path)) {
        return -1;
    }
    user5 = calloc(CPS3_USER5_SIZE / 4, 4);
    if (user5 == NULL) {
        close_set();
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "Out of memory.");
        return -1;
    }
    at = user5;
    ok = build(bios, &cg, put_memory, &at);
    close_set();
    if (!ok) {
        free(user5);
        return -1;
    }
    return cps3_load_rom_data(bios, cg, CG_SIZE, user5);
}

static int put_file(void* ctx, const u32* words, u32 count) {
    if (fwrite(words, 4, count, (FILE*)ctx) != count) {
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "Could not write the graphics ROM.\nThe SD card needs "
                 "about 72 MB free.");
        return 0;
    }
    return 1;
}

static int write_file(const char* path, const void* data, u32 size) {
    FILE* f = fopen(path, "wb");
    int ok = f != NULL && fwrite(data, 1, size, f) == size;
    if (f != NULL && fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "Could not write %.400s.\nThe SD card needs about 72 MB "
                 "free.", path);
    }
    return ok;
}

s32 cps3_unpack_romset(const char* zip_path, const char* dir, void (*progress)(u32 done, u32 total)) {
    static const char* const names[3] = { "user5.bin", "cg.bin", "bios.bin" };    /* bios.bin last */
    static u8 bios[CPS3_BIOS_SIZE];
    char tmp[3][512], final[512];
    u8* cg = NULL;
    FILE* f;
    int ok, i;
    if (!open_set(zip_path)) {
        return -1;
    }
    progress_fn = progress;
    for (i = 0; i < 3; i++) {
        snprintf(tmp[i], sizeof(tmp[i]), "%s/%s.tmp", dir, names[i]);
    }
    f = fopen(tmp[0], "wb");
    if (f == NULL) {
        close_set();
        snprintf(cps3_romset_error, sizeof(cps3_romset_error), "Could not write to %.400s.", dir);
        return -1;
    }
    ok = build(bios, &cg, put_file, f);
    if (fclose(f) != 0) {
        ok = 0;
    }
    close_set();
    ok = ok && write_file(tmp[1], cg, CG_SIZE) && write_file(tmp[2], bios, CPS3_BIOS_SIZE);
    free(cg);
    /* the files appear only once all three are written, so an interrupted unpack starts again */
    for (i = 0; i < 3; i++) {
        snprintf(final, sizeof(final), "%s/%s", dir, names[i]);
        if (ok) {
            remove(final);
            if (rename(tmp[i], final) != 0) {
                snprintf(cps3_romset_error, sizeof(cps3_romset_error), "Could not write %.400s.", final);
                ok = 0;
            }
        }
        if (!ok) {
            remove(tmp[i]);
        }
    }
    return ok ? 0 : -1;
}

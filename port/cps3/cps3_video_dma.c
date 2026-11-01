/*
 * cps3_video_dma.c  Video DMA units: character, colour and sprite list
 *
 * Character and colour DMA raise interrupt level 10 when done.
 * Character RAM is kept in the board's byte order (MAME uses n ^ 3).
 *
 * Derived from MAME's cps3 driver (src/mame/capcom/cps3.cpp), BSD-3-Clause,
 * copyright-holders David Haywood, Andreas Naive, Tomasz Slanina, ElSemi.
 */

#include <string.h>

#include "types.h"
#include "cps3_host.h"

u16 cps3_dma_status;
u32 cps3_spritelist[CPS3_SPRITE_RAM_SIZE / 4] = { 0x80000000 };  /* empty until the first DMA */
u32 cps3_gscroll_buff[8];

#define CHAR_RAM_MASK   0x7FFFFF            /* 8 MB fitted */
#define VREG(ofs)       cps3_be32(cps3_video_reg, ofs)

/* ---- character DMA ---- */

static u32 table_address = 0xFFFFFFFF;      /* decompression table, set by command 4 */
static u8 table[256];                       /* its 128 byte pairs */

u8 cps3_char_dirty[CPS3_CHAR_RAM_SIZE / 256 / 8];

/* Reads graphics ROM bytes in order, one page lookup per page instead of per byte.
   On a little-endian host, ROM byte a is at byte a ^ 1 of the page. */
#define SRC_PAGE_BYTES  (4u << CPS3_USER5_PAGE_SHIFT)
#ifdef HOST_LITTLE_ENDIAN
#define SRC_SWAP        1
#else
#define SRC_SWAP        2
#endif

typedef struct {
    const u8* mem;
    u32 i;                      /* next byte's offset in the page */
    u32 page;
} Src;

static void src_load(Src* s) {
    static const u32 zero_page[1u << CPS3_USER5_PAGE_SHIFT];
    const u32* mem = zero_page;
    if (s->page < CPS3_USER5_PAGES) {
        mem = cps3_user5_page[s->page];
        if (mem == NULL) {
            mem = cps3_user5_fault(s->page);
        }
        cps3_user5_ref[s->page] = 1;
        if (cps3_user5_ahead[s->page]) {
            cps3_user5_ahead_used(s->page);
        }
    }
    s->mem = (const u8*)mem;
}

static void src_open(Src* s, u32 a) {
    s->page = a / SRC_PAGE_BYTES;
    s->i = a % SRC_PAGE_BYTES;
    src_load(s);
}

static inline u8 src_next(Src* s) {
    if (s->i == SRC_PAGE_BYTES) {
        s->page++;
        s->i = 0;
        src_load(s);
    }
    return s->mem[s->i++ ^ SRC_SWAP];
}

/* Marks the written 256-byte blocks for the GPU renderer to upload. */
static void mark_dirty(u32 dst, u32 n) {
    u32 blk, last;
    if (n == 0) {
        return;
    }
    if (n > CHAR_RAM_MASK) {
        n = CHAR_RAM_MASK + 1;
    }
    blk = (dst & CHAR_RAM_MASK) >> 8;
    last = blk + ((dst & 0xFF) + n - 1) / 256;
    for (; blk <= last; blk++) {
        u32 k = blk & (CHAR_RAM_MASK >> 8);
        cps3_char_dirty[k >> 3] |= (u8)(1 << (k & 7));
    }
}

/* memset that wraps at the end of character RAM */
static void char_fill(u32 dst, u8 v, u32 n) {
    dst &= CHAR_RAM_MASK;
    while (n > 0) {
        u32 run = CHAR_RAM_MASK + 1 - dst;
        if (run > n) {
            run = n;
        }
        memset(&cps3_char_ram[dst], v, run);
        n -= run;
        dst = 0;
    }
}

static inline u32 process_byte(u8 real_byte, u32 destination, u8* last_normal_byte) {
    destination &= CHAR_RAM_MASK;
    if (real_byte & 0x40) {
        u32 rle = (real_byte & 0x3F) + 1;
        u32 room = CHAR_RAM_MASK + 1 - destination;    /* stop at the end of character RAM */
        if (rle > room) {
            rle = room;
        }
        if (rle < 16) {
            u8* d = &cps3_char_ram[destination];
            u8 v = *last_normal_byte & 0x3F;
            u32 k;
            for (k = 0; k < rle; k++) {
                d[k] = v;
            }
        } else {
            memset(&cps3_char_ram[destination], *last_normal_byte & 0x3F, rle);
        }
        return rle;
    }
    cps3_char_ram[destination] = real_byte;
    *last_normal_byte = real_byte;
    return 1;
}

static void do_char_dma(u32 src, u32 dst, u32 length);

/* Writes an RLE run. It may write whole words past the run when later output will overwrite them. */
static inline u8* run6(u8* d, const u8* end, u8 code, u8 last) {
    u32 n = (code & 0x3F) + 1;
    u8 v = last & 0x3F;
    if (end - d >= 64) {
        u32 w = v * 0x01010101u;
        u32 k;
        for (k = 0; k < n; k += 8) {
            memcpy(d + k, &w, 4);
            memcpy(d + k + 4, &w, 4);
        }
    } else {
        memset(d, v, n);
    }
    return d + n;
}

static inline u8* emit6(u8* d, const u8* end, u8 x, u8* last) {
    if (x & 0x40) {
        return run6(d, end, x, *last);
    }
    *d = x;
    *last = x;
    return d + 1;
}

/* A 6-bit transfer that can run in pieces, so a slow host can spread scene loads over idle time.
   A renderer that needs a pending tile calls cps3_chr_dma_need. CPU writes to character RAM flush the queue.
   The result is the same as running each transfer at once. */
typedef struct {
    u32 src;                    /* next source byte */
    u32 out;                    /* next output offset in character RAM */
    u32 end;                    /* output end (a run may overshoot by up to 63 bytes) */
    u8 last;                    /* last normal byte, repeated by runs */
    u8 table[256];              /* table at the time the job started */
} ChrJob;

#define CHR_JOBS        32
#define CHR_RUN_OVER    64      /* max run overshoot, rounded up */
#define CHR_DEFER_MIN   0x10000 /* smaller transfers run at once if the queue is empty */

static ChrJob jobs[CHR_JOBS];      /* in the order the game queued them */
u32 cps3_chr_jobs;
int cps3_chr_dma_defer;
u8 cps3_char_pending[CPS3_CHAR_RAM_SIZE / 256 / 8];

/* Runs a job for about `budget` output bytes. Jobs never reach the end of character RAM. */
static void job_run(ChrJob* j, u32 budget) {
    u8* const ram = cps3_char_ram;
    u8* d = ram + j->out;
    u8* const from = d;
    u8* const end = ram + j->end;
    u8* const stop = (j->end - j->out > budget) ? d + budget : end;
    const u8* const tab = j->table;
    u8 last = j->last;
    Src s;
    src_open(&s, j->src);
    while (d < stop) {
        const u8* m;
        u32 i;
        if (s.i == SRC_PAGE_BYTES) {
            s.page++;
            s.i = 0;
            src_load(&s);
        }
        m = s.mem;
        i = s.i;
        do {
            u8 b = m[i++ ^ SRC_SWAP];
            if (!(b & 0xC0)) {
                *d++ = b;
                last = b;
            } else if (b & 0x80) {
                const u8* t = &tab[(b & 0x7F) * 2];
                d = emit6(d, end, t[0], &last);
                if (d >= end) {
                    break;
                }
                d = emit6(d, end, t[1], &last);
            } else {
                d = run6(d, end, b, last);
            }
        } while (i < SRC_PAGE_BYTES && d < stop);
        s.i = i;
    }
    j->src = s.page * SRC_PAGE_BYTES + s.i;
    j->out = (u32)(d - ram);
    j->last = last;
    mark_dirty((u32)(from - ram), (u32)(d - from));
}

/* Does the job still have to write into [lo, hi)? */
static int job_covers(const ChrJob* j, u32 lo, u32 hi) {
    return j->out < hi && lo < j->end + CHR_RUN_OVER;
}

static void pending_rebuild(void) {
    u32 k, t;
    memset(cps3_char_pending, 0, sizeof(cps3_char_pending));
    for (k = 0; k < cps3_chr_jobs; k++) {
        const ChrJob* j = &jobs[k];
        u32 last = (j->end + CHR_RUN_OVER - 1) >> 8;
        for (t = j->out >> 8; t <= last; t++) {
            cps3_char_pending[t >> 3] |= (u8)(1 << (t & 7));
        }
    }
}

static void job_remove(u32 k) {
    cps3_chr_jobs--;
    memmove(&jobs[k], &jobs[k + 1], (cps3_chr_jobs - k) * sizeof(ChrJob));
    pending_rebuild();
}

static void job_finish(u32 k) {
    job_run(&jobs[k], 0xFFFFFFFF);
    job_remove(k);
}

/* Runs queued jobs in order for about `bytes` of output. */
void cps3_chr_dma_work(u32 bytes) {
    while (cps3_chr_jobs > 0) {
        ChrJob* j = &jobs[0];
        u32 before = j->out;
        u32 done;
        job_run(j, bytes);
        done = j->out - before;
        if (j->out >= j->end) {
            job_remove(0);
        }
        if (done >= bytes) {
            return;
        }
        bytes -= done;
    }
}

void cps3_chr_dma_flush(void) {
    while (cps3_chr_jobs > 0) {
        job_finish(0);
    }
}

/* Does just enough queued work to finish one tile. Earlier jobs that overlap are finished first,
   so writes land in the game's order. */
void cps3_chr_dma_need(u32 tile) {
    u32 lo = tile << 8;
    u32 hi = lo + 256;
    while (cps3_char_tile_pending(tile)) {
        u32 k, i;
        ChrJob* j;
        for (k = 0; k < cps3_chr_jobs && !job_covers(&jobs[k], lo, hi); k++) {
        }
        if (k == cps3_chr_jobs) {
            pending_rebuild();          /* marks were stale */
            break;
        }
        j = &jobs[k];
        for (i = 0; i < k && !job_covers(&jobs[i], j->out, j->end + CHR_RUN_OVER); i++) {
        }
        if (i < k) {
            job_finish(i);
            continue;
        }
        job_run(j, hi - j->out);
        if (j->out >= j->end) {
            job_remove(k);
        } else {
            pending_rebuild();
        }
    }
}

/* Queues a 6-bit transfer, or runs it at once. */
static void char_dma6(u32 src, u32 dst, u32 length) {
    ChrJob* j;
    ChrJob now;
    if (dst > CHAR_RAM_MASK + 1 - CHR_RUN_OVER || length > CHAR_RAM_MASK + 1 - CHR_RUN_OVER - dst) {
        /* reaches the end of character RAM (never happens in the game) */
        cps3_chr_dma_flush();
        do_char_dma(src, dst, length);
        return;
    }
    if (!cps3_chr_dma_defer || (length < CHR_DEFER_MIN && cps3_chr_jobs == 0)) {
        /* Small per-frame animation loads run now. Large scene loads are queued. */
        j = &now;
    } else {
        if (cps3_chr_jobs == CHR_JOBS) {
            job_finish(0);
        }
        j = &jobs[cps3_chr_jobs];
    }
    j->src = src;
    j->out = dst;
    j->end = dst + length;
    j->last = 0;
    memcpy(j->table, table, sizeof(table));
    if (j == &now) {
        job_run(j, 0xFFFFFFFF);
    } else {
        cps3_chr_jobs++;
        pending_rebuild();
    }
}

static void do_char_dma(u32 src, u32 dst, u32 length) {
    s32 remaining = length;
    u32 start = dst;
    u8 last_normal_byte = 0;
    Src s;
    src_open(&s, src);
    while (remaining > 0) {
        u8 b = src_next(&s);
        u32 done;
        if (b & 0x80) {
            b &= 0x7F;
            done = process_byte(table[b * 2], dst, &last_normal_byte);
            remaining -= done;
            dst += done;
            if (dst > CHAR_RAM_MASK || remaining <= 0) {
                break;
            }
            done = process_byte(table[b * 2 + 1], dst, &last_normal_byte);
        } else {
            done = process_byte(b, dst, &last_normal_byte);
        }
        remaining -= done;
        dst += done;
        if (dst > CHAR_RAM_MASK) {
            break;
        }
    }
    mark_dirty(start, dst - start);
}

typedef struct {
    u16 lastb, lastb2;
} Last2;

static inline u32 process_byte8(u8 b, u32 dst, Last2* l) {
    if (l->lastb == l->lastb2) {
        u32 rle = (b + 1) & 0xFF;
        char_fill(dst, (u8)l->lastb, rle);
        l->lastb2 = 0xFFFF;
        return rle;
    }
    l->lastb2 = l->lastb;
    l->lastb = b;
    cps3_char_ram[dst & CHAR_RAM_MASK] = b;
    return 1;
}

static void do_alt_char_dma(u32 src, u32 dst, u32 length) {
    u32 start = dst;
    Last2 l = { 0xFFFE, 0xFFFF };
    Src s;
    src_open(&s, src);
    while (1) {
        u8 ctrl = src_next(&s);
        s32 i;
        for (i = 0; i < 8; i++) {
            u8 p = src_next(&s);
            if (ctrl & 0x80) {
                p &= 0x7F;
                dst += process_byte8(table[p * 2], dst, &l);
                dst += process_byte8(table[p * 2 + 1], dst, &l);
            } else {
                dst += process_byte8(p, dst, &l);
            }
            ctrl <<= 1;
            if (dst - start >= length) {
                mark_dirty(start, dst - start);
                return;
            }
        }
    }
}

/* A DMA list word, as the CPU wrote it. */
static u32 list_word(u32 index) {
    u32 v;
    memcpy(&v, &cps3_char_ram[(index * 4) & (CPS3_CHAR_RAM_SIZE - 4)], 4);
    return v;
}

/* If queued jobs would overwrite the new list, finish them and restore the list. */
static void keep_list(u32 address) {
    static u32 saved[0x1000];
    u32 n, k;
    int hit = 0;
    for (n = 0; n < 0x1000 - 2; n += 3) {
        if (list_word(address + n) & 0x01000000) {
            break;
        }
    }
    n++;                                    /* include the end word */
    for (k = 0; k < n && !hit; k++) {
        hit = cps3_char_tile_pending((((address + k) * 4) & (CPS3_CHAR_RAM_SIZE - 4)) >> 8);
    }
    if (!hit) {
        return;
    }
    for (k = 0; k < n; k++) {
        saved[k] = list_word(address + k);
    }
    cps3_chr_dma_flush();
    for (k = 0; k < n; k++) {
        memcpy(&cps3_char_ram[((address + k) * 4) & (CPS3_CHAR_RAM_SIZE - 4)], &saved[k], 4);
    }
}

/* List word address: low half at VIDEO_REG + 0x96, high 6 bits at 0x98. Bit 6 of 0x98 starts it. */
void cps3_chr_dma_start(void) {
    u16 hi = cps3_video_reg[0x98 / 2];
    u32 address;
    s32 i;
    if (!(hi & 0x40)) {
        return;
    }
    address = cps3_video_reg[0x96 / 2] | ((u32)(hi & 0x3F) << 16);
    if (cps3_chr_jobs > 0) {
        keep_list(address);
    }
    for (i = 0; i < 0x1000; i += 3) {
        u32 dat1 = list_word(address + i);
        u32 dat2 = list_word(address + i + 1);
        u32 dat3 = list_word(address + i + 2);
        u32 length = ((dat1 & 0x1FFFFF) + 1) << 3;
        u32 dst = dat2 << 3;
        u32 src = (dat3 << 1) - 0x400000;
        if (dat1 & 0x01000000) {
            break;
        }
        switch ((dat1 >> 21) & 7) {
        case 4: {
            Src s;
            u32 k;
            src_open(&s, src);
            table_address = src;
            for (k = 0; k < sizeof(table); k++) {
                table[k] = src_next(&s);
            }
            break;
        }
        case 2:
            char_dma6(src, dst, length);
            break;
        case 3:
            cps3_chr_dma_flush();
            do_alt_char_dma(src, dst, length);
            break;
        case 0: {
            Src s;
            u32 k;
            cps3_chr_dma_flush();
            src_open(&s, src);
            for (k = 0; k < length; k++) {
                cps3_char_ram[(dst + k) & CHAR_RAM_MASK] = src_next(&s);
            }
            mark_dirty(dst, length);
            break;
        }
        default:
            break;
        }
    }
    cps3_dma_end_irq();
}

/* ---- colour DMA ---- */

static u16 fade_channel(u16 c, u32 f) {
    if (f & 0x40) {
        c = (f & 0x20) ? ((((c ^ 0x1F) * (~f & 0x1F)) >> 5) ^ 0x1F) : (c * (f & 0x1F) >> 5);
    }
    return c;
}

static u16 fade_colour(u16 data, u32 fade) {
    if (fade & 0x40400040) {
        u16 r = fade_channel(data & 0x1F, (fade >> 24) & 0x7F);
        u16 g = fade_channel((data >> 5) & 0x1F, (fade >> 16) & 0x7F);
        u16 b = fade_channel((data >> 10) & 0x1F, fade & 0x7F);
        data = (data & 0x8000) | r | (g << 5) | (b << 10);
    }
    return data;
}

/* VIDEO_REG + 0xA0..0xAF: source, destination, fade, length. Bit 1 of 0xAE starts it. */
void cps3_col_dma_start(void) {
    u32 ctrl = VREG(0xAC);
    u32 src;
    u32 dst;
    u32 fade;
    u32 length;
    u32 i;
    if (!(ctrl & 2)) {
        return;
    }
    src = ((VREG(0xA0) << 1) - 0x400000) >> 1;
    dst = VREG(0xA4);
    fade = VREG(0xA8);
    length = (ctrl >> 16) | ((ctrl & 1) << 16);
    for (i = 0; i < length; i++) {
        cps3_color_ram[((dst + i) ^ 1) & 0x1FFFF] = fade_colour(cps3_user5_word(src + i), fade);
    }
    cps3_dma_end_irq();
}

/* ---- sprite list DMA ---- */

/* Latches the sprite list, its sublists and the global scroll registers for the frame. */
void cps3_sprite_dma(void) {
    u32 i;
    u32 k;
    for (i = 0; i < 0x2000 / 4; i += 4) {
        u32 dat;
        u32 offs;
        u32 length;
        for (k = 0; k < 4; k++) {
            cps3_spritelist[i + k] = cps3_be32(cps3_sprite_ram, (i + k) * 4);
        }
        dat = cps3_spritelist[i];
        if (dat & 0x80000000) {
            break;
        }
        offs = (dat & 0x7FFF) << 2;
        length = (dat & 0x01FF0000) >> 16;
        for (k = 0; k < length * 4 && offs + k < CPS3_SPRITE_RAM_SIZE / 4; k++) {
            cps3_spritelist[offs + k] = cps3_be32(cps3_sprite_ram, (offs + k) * 4);
        }
    }
    for (k = 0; k < 8; k++) {
        cps3_gscroll_buff[k] = VREG(k * 4);
    }
}

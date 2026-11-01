/*
 * cps3_sound.c  CPS3 sound chip: 16 voices of signed 8-bit PCM, mixed to stereo
 *
 * The game's sound driver handles envelopes and panning by rewriting voice registers each frame.
 * Voice registers are 16-bit words at sound registers + voice * 0x20:
 *
 *   word 2, 3     start address (low, high)
 *   word 5        bit 0: loop
 *   word 6        pitch, 4.12 fixed point
 *   word 7, 9     loop address (low, high)
 *   word 10, 11   end address (low, high)
 *   word 14, 15   left and right volume
 *
 * The key-on register at + 0x200 has a bit per voice. A non-looping voice clears its bit at the end.
 * Sample ROM addresses start at 0x400000 in user5.
 *
 * The voice model follows MAME's cps3 sound device (src/mame/capcom/cps3_a.cpp), BSD-3-Clause,
 * copyright-holders David Haywood, Andreas Naive, Tomasz Slanina, ElSemi.
 */

#include <string.h>

#include "cps3_host.h"
#include "cps3_sound.h"

#define SAMPLE_BASE 0x400000u   /* sample ROM start in user5 */
#define KEY_WORD    0x100       /* key-on register index in cps3_sound_reg */

typedef struct {
    u32 pos;                    /* bytes from the start address */
    u32 frac;                   /* 12-bit fraction */
} Voice;

static Voice voices[16];
static u32 key;

#define REG(v, w) (cps3_sound_reg[(v) * 16 + (w)])

void cps3_sound_reset(void) {
    memset(voices, 0, sizeof(voices));
    key = 0;
    cps3_sound_reg[KEY_WORD] = 0;
}

void cps3_sound_key_write(u32 k) {
    u32 i;
    k &= 0xFFFF;
    for (i = 0; i < 16; i++) {
        if ((k & (1u << i)) && !(key & (1u << i))) {
            voices[i].pos = 0;
            voices[i].frac = 0;
        }
    }
    key = k;
    cps3_sound_reg[KEY_WORD] = (u16)k;
}

/* On a little-endian host, sample ROM byte n is byte n in memory (unlike the graphics bytes).
   When streaming, a voice whose page is not loaded yet stays silent until it arrives. */
#define PAGE_SHIFT  (CPS3_USER5_PAGE_SHIFT + 2)     /* bytes */
#define PAGE_WORDS  (1u << CPS3_USER5_PAGE_SHIFT)

/* The page holding sample ROM byte a, or NULL while it loads. */
static const u32* sample_page(u32 a) {
    static const u32 silence[PAGE_WORDS];
    u32 page = (a - SAMPLE_BASE) >> PAGE_SHIFT;
    if (page >= CPS3_USER5_PAGES) {
        return silence;
    }
    if (!cps3_user5_ready(page)) {
        return NULL;
    }
    cps3_user5_soon(page + 1);
    return cps3_user5_page[page];
}

void cps3_sound_render(s16* out, u32 n) {
    static s32 mix[2 * 1024];
    u32 i, j, done = 0;
    /* A direct write to the register (as on reset) can only key voices off. */
    key &= cps3_sound_reg[KEY_WORD];
    while (done < n) {
        u32 len = n - done > 1024 ? 1024 : n - done;
        memset(mix, 0, len * 2 * sizeof(s32));
        for (i = 0; i < 16; i++) {
            Voice* v = &voices[i];
            u32 start, end, loop, step, pos, frac, page = ~0u;
            const u32* mem = NULL;
            s32 vol_l, vol_r;
            int looping;
            if (!(key & (1u << i))) {
                continue;
            }
            start = REG(i, 2) | ((u32)REG(i, 3) << 16);
            end = REG(i, 10) | ((u32)REG(i, 11) << 16);
            loop = REG(i, 7) | ((u32)REG(i, 9) << 16);
            looping = REG(i, 5) & 1;
            step = REG(i, 6);
            vol_l = (s16)REG(i, 14);
            vol_r = (s16)REG(i, 15);
            pos = v->pos;
            frac = v->frac;
            j = 0;
            while (j < len) {
                /* Find the next sample, then mix the run that stays in this page without rechecking. */
                u32 p = pos + (frac >> 12), f = frac & 0xFFF, off, lim, delta;
                const s8* bytes;
                if (start + p >= end) {
                    if (looping) {
                        p = p + loop - end;         /* carry the overshoot into the loop */
                    } else {
                        key &= ~(1u << i);
                        pos = p;
                        frac = f;
                        break;
                    }
                }
                off = start + p - SAMPLE_BASE;
                if (off >> PAGE_SHIFT != page) {
                    mem = sample_page(start + p);
                    if (mem == NULL) {
                        /* wait for the page */
                        break;
                    }
                    page = off >> PAGE_SHIFT;
                }
                delta = off - p - (page << PAGE_SHIFT);     /* p + delta is the byte in the page */
                if (page >= CPS3_USER5_PAGES || end <= start) {
                    lim = p + 1;                    /* outside the ROM: one sample at a time */
                } else {
                    lim = (1u << PAGE_SHIFT) - delta;   /* p at the page's end */
                    if (end - start < lim) {
                        lim = end - start;
                    }
                }
                bytes = (const s8*)mem;
                do {
                    s32 s = bytes[p + delta];
                    pos = p;
                    frac = f + step;
                    mix[j * 2] += (s * vol_l) >> 8;
                    mix[j * 2 + 1] += (s * vol_r) >> 8;
                    j++;
                    p = pos + (frac >> 12);
                    f = frac & 0xFFF;
                } while (j < len && p < lim);
            }
            v->pos = pos;
            v->frac = frac;
        }
        for (j = 0; j < len * 2; j++) {
            s32 s = mix[j];
            out[done * 2 + j] = (s16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        done += len;
    }
    cps3_sound_reg[KEY_WORD] = (u16)key;
}

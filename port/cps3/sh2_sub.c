/*
 * sh2_sub.c  SH-2 helpers from sh2_sub.src
 *
 * dma0_transfer_wait is only used by the test menu's ROM check. The host has no flash there, so it
 * writes zeros.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3.h"

u32 rotate_right(u32 v) {
    return (v >> 1) | (v << 31);
}

u32 rotate_left(u32 v) {
    return (v << 1) | (v >> 31);
}

u32 rotate_right_n(u32 v, s32 n) {
    do {
        v = rotate_right(v);
    } while (--n != 0);
    return v;
}

u32 rotate_left_n(u32 v, s32 n) {
    do {
        v = rotate_left(v);
    } while (--n != 0);
    return v;
}

void delay_cycles(s32 count) {
    (void)count;
}

void jump_to(void (*addr)(void)) {
    addr();
}

volatile u32* dma0_transfer_wait(u32 src, u32 dst, u32 count, u32 size) {
    static const u32 unit[4] = { 1, 2, 4, 16 };
    (void)src;
    memset((void*)(uintptr_t)dst, 0, (count & 0xFFFFFF) * (size == 3 ? 4 : unit[size & 3]));
    return (volatile u32*)SH2_DMAOR;
}

/*
 * ctr_bottom.c  The bottom screen: the chosen fighter's move list (3DS)
 *
 * The pictures are in the romfs (romfs:/bottom/<name>.z, made by port/tools/bottom_screens.py): zlib-compressed
 * RGB565 in framebuffer order. A thread unpacks the next one while the game runs, and it is copied to the screen
 * when ready.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <zlib.h>

#include "types.h"
#include "ctr_bottom.h"

#define FB_PIXELS   (320 * 240)
#define FB_BYTES    (FB_PIXELS * 2)
#define COMP_MAX    (128 * 1024)

/* by fighter number (fighter.h); Shin Gouki uses Gouki's */
static const char* const names[CTR_BOTTOM_FIGHTERS] = {
    "gill", "alex", "ryu", "yun", "dudley", "necro", "hugo", "ibuki", "elena", "oro", "yang",
    "ken", "sean", "urien", "akuma", "akuma", "chunli", "makoto", "q", "twelve", "remy",
};

static u16* fb;
static u16* staging;                /* the next picture, unpacked */
static u8* comp;
static int available;
static Thread thread;
static LightEvent wake;
static volatile int quit;
static volatile int wanted = -2;    /* -1: the default picture, else a fighter; -2: none yet */
static volatile int loaded = -2;    /* what staging holds (-2 while it is being written) */
static int shown = -3;

static int load(int which) {
    char path[64];
    FILE* f;
    size_t n;
    uLongf out = FB_BYTES;
    snprintf(path, sizeof(path), "romfs:/bottom/%s.z", which < 0 ? "default" : names[which]);
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    n = fread(comp, 1, COMP_MAX, f);
    fclose(f);
    return uncompress((Bytef*)staging, &out, comp, n) == Z_OK && out == FB_BYTES;
}

static void loader_main(void* arg) {
    (void)arg;
    while (!quit) {
        int w = wanted;
        if (w == -2 || w == loaded) {
            LightEvent_Wait(&wake);
            continue;
        }
        loaded = -2;
        if (!load(w) && !(w >= 0 && load(-1))) {
            memset(staging, 0, FB_BYTES);
        }
        loaded = w;
    }
}

int ctr_bottom_init(u16* framebuffer) {
    s32 prio = 0x30;
    FILE* f;
    if (R_FAILED(romfsInit())) {
        return 0;
    }
    f = fopen("romfs:/bottom/default.z", "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    staging = malloc(FB_BYTES);
    comp = malloc(COMP_MAX);
    if (staging == NULL || comp == NULL) {
        return 0;
    }
    fb = framebuffer;
    LightEvent_Init(&wake, RESET_ONESHOT);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    thread = threadCreate(loader_main, NULL, 16 * 1024, prio + 1, -2, false);
    if (thread == NULL) {
        return 0;
    }
    available = 1;
    return 1;
}

void ctr_bottom_frame(int fighter) {
    int want = fighter >= 0 && fighter < CTR_BOTTOM_FIGHTERS ? fighter : -1;
    if (!available) {
        return;
    }
    if (want != wanted) {
        wanted = want;
        LightEvent_Signal(&wake);
    }
    if (loaded == want && shown != want) {
        memcpy(fb, staging, FB_BYTES);
        GSPGPU_FlushDataCache(fb, FB_BYTES);
        shown = want;
    }
}

void ctr_bottom_exit(void) {
    if (thread != NULL) {
        quit = 1;
        LightEvent_Signal(&wake);
        threadJoin(thread, U64_MAX);
        threadFree(thread);
        thread = NULL;
    }
    romfsExit();
}

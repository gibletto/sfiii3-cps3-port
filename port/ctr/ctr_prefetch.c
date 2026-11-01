/*
 * ctr_prefetch.c  Reads the graphics ROM ahead of the game (3DS)
 *
 * A page missing from the cache takes about 7 ms to read from the SD card. The game mostly reads the ROM in
 * order, so after each miss a low-priority thread reads the next few pages into spare buffers, and they are
 * moved into the cache between frames.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "types.h"
#include "cps3_host.h"
#include "ctr_prefetch.h"

#define PAGE_BYTES  (4u << CPS3_USER5_PAGE_SHIFT)
#define BUFS        12
#define AHEAD       8

enum { B_FREE, B_QUEUED, B_READING, B_DONE };

typedef struct {
    u32 page;
    u32 age;                /* queue order: the oldest is reused first */
    u32 urgent;             /* wanted by the sound: read first */
    volatile u32 state;
    u8* data;
} Buf;

static Buf bufs[BUFS];
static LightLock lock;
static LightEvent wake;
static Thread thread;
static volatile int quit;
static FILE* file;
static u32 file_size;
static u32 queue_clock;

static void read_page(Buf* b) {
    u32 ofs = b->page * PAGE_BYTES;
    u32 want = ofs + PAGE_BYTES <= file_size ? PAGE_BYTES : file_size - ofs;
    memset(b->data, 0, PAGE_BYTES);
    fseek(file, ofs, SEEK_SET);
    if (fread(b->data, 1, want, file) != want) {
        memset(b->data, 0, PAGE_BYTES);
    }
}

static void reader_main(void* arg) {
    (void)arg;
    while (!quit) {
        Buf* next = NULL;
        u32 i;
        LightLock_Lock(&lock);
        for (i = 0; i < BUFS; i++) {
            if (bufs[i].state == B_QUEUED &&
                (next == NULL || bufs[i].urgent > next->urgent ||
                 (bufs[i].urgent == next->urgent && bufs[i].age < next->age))) {
                next = &bufs[i];
            }
        }
        if (next != NULL) {
            next->state = B_READING;
        }
        LightLock_Unlock(&lock);
        if (next == NULL) {
            LightEvent_Wait(&wake);
            continue;
        }
        read_page(next);
        LightLock_Lock(&lock);
        next->state = B_DONE;
        LightLock_Unlock(&lock);
    }
}

static Buf* find(u32 page) {
    u32 i;
    for (i = 0; i < BUFS; i++) {
        if (bufs[i].state != B_FREE && bufs[i].page == page) {
            return &bufs[i];
        }
    }
    return NULL;
}

/* a cache miss: hand over the page if a buffer has it or is reading it */
static int take(u32 page, void* dst) {
    Buf* b;
    LightLock_Lock(&lock);
    b = find(page);
    if (b != NULL && b->state == B_QUEUED) {
        b->state = B_FREE;              /* not started yet: the cache reads it itself */
        b->urgent = 0;
        b = NULL;
    }
    LightLock_Unlock(&lock);
    if (b == NULL) {
        return 0;
    }
    while (b->state == B_READING) {
        svcSleepThread(100000);
    }
    memcpy(dst, b->data, PAGE_BYTES);
    LightLock_Lock(&lock);
    b->state = B_FREE;
    b->urgent = 0;
    LightLock_Unlock(&lock);
    return 1;
}

/* after a miss: queue the pages that follow it */
static void ahead(u32 page) {
    u32 d, i;
    int queued = 0;
    LightLock_Lock(&lock);
    for (d = 1; d <= AHEAD; d++) {
        u32 p = page + d;
        Buf* b = NULL;
        if (p >= CPS3_USER5_PAGES || p * PAGE_BYTES >= file_size || cps3_user5_cached(p) || find(p) != NULL) {
            continue;
        }
        for (i = 0; i < BUFS && b == NULL; i++) {
            if (bufs[i].state == B_FREE) {
                b = &bufs[i];
            }
        }
        if (b == NULL) {
            /* none free: reuse the oldest that is not being read */
            for (i = 0; i < BUFS; i++) {
                if (bufs[i].state != B_READING && !bufs[i].urgent && (b == NULL || bufs[i].age < b->age)) {
                    b = &bufs[i];
                }
            }
        }
        if (b == NULL) {
            break;
        }
        b->page = p;
        b->age = ++queue_clock;
        b->urgent = 0;
        b->state = B_QUEUED;
        queued = 1;
    }
    LightLock_Unlock(&lock);
    if (queued) {
        LightEvent_Signal(&wake);
    }
}

/* a page the sound wants (it plays on without waiting): read it before the others */
static void want(u32 page) {
    Buf* b;
    u32 i;
    if (page >= CPS3_USER5_PAGES || page * PAGE_BYTES >= file_size) {
        return;
    }
    LightLock_Lock(&lock);
    b = find(page);
    if (b != NULL) {
        b->urgent = 1;
        LightLock_Unlock(&lock);
        LightEvent_Signal(&wake);
        return;
    }
    for (i = 0; i < BUFS && b == NULL; i++) {
        if (bufs[i].state == B_FREE) {
            b = &bufs[i];
        }
    }
    for (i = 0; i < BUFS && b == NULL; i++) {
        if (bufs[i].state == B_QUEUED && !bufs[i].urgent) {
            b = &bufs[i];
        }
    }
    if (b != NULL) {
        b->page = page;
        b->age = ++queue_clock;
        b->urgent = 1;
        b->state = B_QUEUED;
    }
    LightLock_Unlock(&lock);
    if (b != NULL) {
        LightEvent_Signal(&wake);
    }
}

static const Cps3User5Reader reader = { take, ahead, want };

void ctr_prefetch_poll(void) {
    u32 i;
    if (thread == NULL) {
        return;
    }
    for (i = 0; i < BUFS; i++) {
        if (bufs[i].state == B_DONE) {
            cps3_user5_install(bufs[i].page, bufs[i].data);
            LightLock_Lock(&lock);
            bufs[i].state = B_FREE;
            bufs[i].urgent = 0;
            LightLock_Unlock(&lock);
        }
    }
}

int ctr_prefetch_start(void) {
    const char* path = cps3_user5_path();
    s32 prio = 0x30;
    u32 i;
    if (path == NULL) {
        return 0;                       /* the whole ROM is in memory */
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    file_size = cps3_user5_file_size();
    for (i = 0; i < BUFS; i++) {
        bufs[i].data = malloc(PAGE_BYTES);
        bufs[i].state = B_FREE;
        if (bufs[i].data == NULL) {
            return 0;
        }
    }
    LightLock_Init(&lock);
    LightEvent_Init(&wake, RESET_ONESHOT);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    thread = threadCreate(reader_main, NULL, 16 * 1024, prio + 1, -2, false);
    if (thread == NULL) {
        return 0;
    }
    cps3_user5_set_reader(&reader);
    return 1;
}

void ctr_prefetch_stop(void) {
    if (thread != NULL) {
        cps3_user5_set_reader(NULL);
        quit = 1;
        LightEvent_Signal(&wake);
        threadJoin(thread, U64_MAX);
        threadFree(thread);
        thread = NULL;
    }
}

/*
 * ctr_prefetch.c  Reads the graphics ROM ahead of the game (3DS)
 *
 * A page missing from the cache takes about 4.5 ms to read from the SD card (16 KB, straight through FS). The
 * game mostly reads the ROM in order, so after each miss a thread reads the next few pages into spare buffers,
 * and they are moved into the cache between frames. It also reads the pages that queued graphics loads will need
 * (cps3_chr_dma_upcoming). The thread runs on the system core when it can, so its reads overlap the game.
 *
 * With nothing else to read, the thread also fills the cache slots that have never been used with the pages
 * nobody has asked for yet, a few at a time, so later scenes find their graphics loaded. It never evicts a page,
 * and it waits while the game is missing pages (a scene is loading). When the whole ROM fits, it is all in memory
 * about ten seconds after the start.
 *
 * The thread also writes the settings EEPROM when the game changes it, so the SD card write does not stall a frame.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "types.h"
#include "cps3_host.h"
#include "ctr_prefetch.h"

#define PAGE_BYTES  (4u << CPS3_USER5_PAGE_SHIFT)
#define BUFS        24
#define AHEAD       8

enum { B_FREE, B_QUEUED, B_READING, B_DONE };

typedef struct {
    u32 page;
    u32 age;                /* queue order: the oldest is reused first */
    u32 urgent;             /* wanted by the sound or a graphics load: read first */
    volatile u32 state;
    u8* data;
} Buf;

static Buf bufs[BUFS];
static LightLock lock;
static LightEvent wake;
static Thread thread;
static volatile int quit;
static Handle fh_thread, fh_main;   /* the graphics ROM file, opened once for each thread */
static u32 file_size;
static u32 queue_clock;

/* the background fill: one job of up to FILL_PAGES pages in a row, read in one request */
#define FILL_PAGES  4
#define FILL_QUIET  30          /* frames without a miss before the fill goes on */
enum { F_IDLE, F_QUEUED, F_READING, F_DONE, F_FINISHED };
static struct {
    volatile u32 state;
    u32 first, n;
    s32 slot[FILL_PAGES];
    u32 next;                   /* the next page to look at */
    u32 last_misses;
    u32 quiet;
    u8* buf;
} fill;
static Buf* find(u32 page);

/* reads a page straight through FS (stdio costs about 2.7 ms more per 16 KB read) */
static int raw_read(Handle h, u32 page, void* dst) {
    u32 ofs = page * PAGE_BYTES;
    u32 want = ofs + PAGE_BYTES <= file_size ? PAGE_BYTES : file_size - ofs;
    u32 got = 0;
    if (want < PAGE_BYTES) {
        memset(dst, 0, PAGE_BYTES);
    }
    if (R_FAILED(FSFILE_Read(h, &got, ofs, dst, want)) || got != want) {
        memset(dst, 0, PAGE_BYTES);
        return 0;
    }
    return 1;
}

/* a cache miss with nothing read ahead: the game thread reads it now */
static int read_now(u32 page, void* dst) {
    return page * PAGE_BYTES < file_size && raw_read(fh_main, page, dst);
}

static int open_raw(const char* path, Handle* h) {
    return strncmp(path, "sdmc:", 5) == 0 &&
           R_SUCCEEDED(FSUSER_OpenFileDirectly(h, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                                               fsMakePath(PATH_ASCII, path + 5), FS_OPEN_READ, 0));
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
        } else if (fill.state == F_QUEUED) {
            fill.state = F_READING;
        }
        LightLock_Unlock(&lock);
        if (next == NULL && fill.state == F_READING) {
            u32 k, got = 0, want = fill.n * PAGE_BYTES, ofs = fill.first * PAGE_BYTES;
            if (ofs + want > file_size) {
                want = file_size - ofs;
                memset(fill.buf + want, 0, fill.n * PAGE_BYTES - want);
            }
            if (R_FAILED(FSFILE_Read(fh_thread, &got, ofs, fill.buf, want)) || got != want) {
                fill.n = 0;             /* the slots go back unused */
            }
            for (k = 0; k < fill.n; k++) {
                memcpy(cps3_user5_slot_mem((u32)fill.slot[k]), fill.buf + k * PAGE_BYTES, PAGE_BYTES);
            }
            LightLock_Lock(&lock);
            fill.state = F_DONE;
            LightLock_Unlock(&lock);
            continue;
        }
        if (next == NULL) {
            if (!cps3_eeprom_flush()) {
                LightEvent_Wait(&wake);
            }
            continue;
        }
        raw_read(fh_thread, next->page, next->data);
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

/* a page wanted soon by something that does not wait for it (the sound, a queued graphics load): read it first */
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

static const Cps3User5Reader reader = { take, ahead, want, read_now };

/* the pages queued graphics loads will read: into free buffers only, in the order they are needed */
static void hint_chr_loads(void) {
    u32 pages[24], n, k, i;
    int queued = 0;
    n = cps3_chr_dma_upcoming(pages, 24);
    if (n == 0) {
        return;
    }
    LightLock_Lock(&lock);
    for (k = 0; k < n; k++) {
        u32 p = pages[k];
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

/* between frames: hands in the last fill job and, when the game has not missed a page for a while, queues the next */
static void fill_poll(void) {
    u32 k, pages;
    if (fill.state == F_FINISHED || fill.buf == NULL) {
        return;
    }
    if (fill.state == F_DONE) {
        for (k = 0; k < fill.n; k++) {
            cps3_user5_fill((u32)fill.slot[k], fill.first + k);
        }
        for (; k < FILL_PAGES && fill.slot[k] >= 0; k++) {
            cps3_user5_fill((u32)fill.slot[k], CPS3_USER5_PAGES);      /* a failed read: give the slot back */
        }
        fill.state = F_IDLE;
    }
    if (fill.state != F_IDLE) {
        return;
    }
    if (cps3_user5_misses != fill.last_misses) {
        fill.last_misses = cps3_user5_misses;
        fill.quiet = FILL_QUIET;
    }
    if (fill.quiet != 0) {
        fill.quiet--;
        return;
    }
    pages = (file_size + PAGE_BYTES - 1) / PAGE_BYTES;
    if (pages > CPS3_USER5_PAGES) {
        pages = CPS3_USER5_PAGES;
    }
    LightLock_Lock(&lock);
    while (fill.next < pages && (cps3_user5_cached(fill.next) || find(fill.next) != NULL)) {
        fill.next++;
    }
    fill.first = fill.next;
    fill.n = 0;
    while (fill.n < FILL_PAGES && fill.next < pages && !cps3_user5_cached(fill.next) && find(fill.next) == NULL) {
        s32 slot = cps3_user5_spare_slot();
        if (slot < 0) {
            break;
        }
        fill.slot[fill.n++] = slot;
        fill.next++;
    }
    for (k = fill.n; k < FILL_PAGES; k++) {
        fill.slot[k] = -1;
    }
    if (fill.n == 0) {
        fill.state = F_FINISHED;        /* every page loaded, or no slot left */
        LightLock_Unlock(&lock);
        free(fill.buf);
        fill.buf = NULL;
        return;
    }
    fill.state = F_QUEUED;
    LightLock_Unlock(&lock);
    LightEvent_Signal(&wake);
}

void ctr_prefetch_poll(void) {
    u32 i;
    if (thread == NULL) {
        return;
    }
    fill_poll();
    if (cps3_eeprom_pending()) {
        LightEvent_Signal(&wake);       /* the thread saves it */
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
    hint_chr_loads();
}

int ctr_prefetch_start(void) {
    const char* path = cps3_user5_path();
    s32 prio = 0x30;
    u32 i;
    if (path == NULL) {
        return 0;                       /* the whole ROM is in memory */
    }
    if (!open_raw(path, &fh_thread)) {
        return 0;
    }
    if (!open_raw(path, &fh_main)) {
        FSFILE_Close(fh_thread);
        return 0;
    }
    file_size = cps3_user5_file_size();
    fill.buf = malloc(FILL_PAGES * PAGE_BYTES);     /* optional: without it, no background fill */
    fill.state = F_IDLE;
    fill.quiet = FILL_QUIET;
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
    /* The system core (core 1): the app may use up to the time limit there. The reads mostly wait on the SD card,
       so they need little of it. Otherwise the game's core, above the game: it takes the CPU only to start a
       read, instead of waiting until the game is idle. */
    if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(30))) {
        thread = threadCreate(reader_main, NULL, 16 * 1024, prio - 1, 1, false);
    }
    if (thread == NULL) {
        thread = threadCreate(reader_main, NULL, 16 * 1024, prio - 1, -2, false);
    }
    if (thread == NULL) {
        return 0;
    }
    cps3_user5_set_reader(&reader);
    cps3_eeprom_defer(1);
    return 1;
}

void ctr_prefetch_stop(void) {
    if (thread != NULL) {
        cps3_user5_set_reader(NULL);
        fill.state = F_FINISHED;
        quit = 1;
        LightEvent_Signal(&wake);
        threadJoin(thread, U64_MAX);
        threadFree(thread);
        thread = NULL;
        FSFILE_Close(fh_thread);
        FSFILE_Close(fh_main);
        cps3_eeprom_defer(0);
        cps3_eeprom_flush();            /* a change not saved yet */
    }
}

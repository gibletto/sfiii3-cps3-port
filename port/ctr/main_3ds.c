/*
 * main_3ds.c  The 3DS front end (libctru, citro3d)
 *
 * The ROM files go in sdmc:/sf3/rom-data (made by port/tools/prepare_rom.py); the EEPROM is saved there too.
 * The 64 MB graphics ROM does not fit in an Old 3DS's memory, so it is streamed from the SD card through a
 * page cache, with a thread reading ahead (ctr_prefetch.c).
 *
 * Top screen: the game, drawn by the GPU (ctr_video.c). Bottom screen: the chosen fighter's move list
 * (ctr_bottom.c). Sound: the DSP (ctr_sound.c).
 *
 * Controls: D-pad or Circle Pad to move; the other buttons are mapped in sdmc:/sf3/controls.ini (ctr_controls.c).
 * SELECT inserts a coin; held with START it is the test switch (settings, free play), with L the service coin,
 * with R the button setup screen, with B it quits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3_kernel.h"
#include "ctr_video.h"
#include "ctr_prefetch.h"
#include "ctr_sound.h"
#include "ctr_bottom.h"
#include "ctr_controls.h"

#define ROM_DIR         "sdmc:/sf3/rom-data"
#define USER5_PAGES     4096        /* graphics ROM cache in 16 KB pages: as many as memory allows */
#define USER5_RESERVE   (4u << 20)  /* memory left free for later allocations (task stacks) */

static PrintConsole con;
static u32 coin_frames;             /* a coin holds the switch for a few frames */
static int select_combo;            /* SELECT was used with another button: no coin on release */
static int pictures;                /* the bottom screen shows the move lists */

enum { QUIT, PLAY, SETUP };

/* reads the buttons into the board's switches */
static int read_inputs(void) {
    static const struct {
        u32 key;
        u32 in;
    } dirs[] = {
        { KEY_UP, CPS3_IN_UP }, { KEY_DOWN, CPS3_IN_DOWN }, { KEY_LEFT, CPS3_IN_LEFT }, { KEY_RIGHT, CPS3_IN_RIGHT },
    };
    const u32 shifted = KEY_START | KEY_L | KEY_R | KEY_B;
    u32 held = hidKeysHeld();
    u32 down = hidKeysDown();
    u32 p1 = 0, sys = 0;
    u32 i;
    if (held & KEY_SELECT) {
        if (down & KEY_B) {
            return QUIT;
        }
        if (down & KEY_R) {
            select_combo = 0;
            return SETUP;
        }
        if (held & KEY_START) {
            sys |= CPS3_SYS_TEST;
        }
        if (held & KEY_L) {
            sys |= CPS3_SYS_SERVICE;
        }
        if (held & shifted) {
            select_combo = 1;
        }
        held &= ~shifted;
    } else if (hidKeysUp() & KEY_SELECT) {
        if (!select_combo) {
            coin_frames = 6;
        }
        select_combo = 0;
    }
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (held & dirs[i].key) {
            p1 |= dirs[i].in;
        }
    }
    p1 |= ctr_controls_map(held);
    if (coin_frames != 0) {
        coin_frames--;
        p1 |= CPS3_IN_COIN;
    }
    cps3_set_inputs(p1, 0, sys);
    return PLAY;
}

/* the fighter to show on the bottom screen: the player's confirmed choice, or -1 */
static int player_fighter(void) {
    extern u8 My_char[2];
    extern s8 Operator_Status[2];
    extern s16 Sel_PL_Complete[2];
    int p = Operator_Status[0] ? 0 : Operator_Status[1] ? 1 : -1;
    if (p < 0 || Sel_PL_Complete[p] == 0) {
        return -1;      /* until confirmed, My_char is only the cursor */
    }
    return My_char[p];
}

/* waits up to `frames` frames for a button; with frames 0, waits for START */
static void wait_key(u32 frames) {
    u32 n = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (frames == 0 ? (hidKeysDown() & KEY_START) != 0 : (hidKeysDown() != 0 || ++n >= frames)) {
            break;
        }
        gspWaitForVBlank();
    }
}

static int fail(const char* message) {
    printf("\n%s\n\nPress START to quit.\n", message);
    wait_key(0);
    gfxExit();
    return 0;
}

/* the bottom screen's text, under the pictures if there are any */
static void bottom_help(void) {
    consoleClear();
    if (!pictures) {
        printf("\nSELECT coin, SELECT+START test menu\nSELECT+L service, SELECT+R buttons\nSELECT+B quit\n");
    }
}

int main(int argc, char** argv) {
    char path[256];
    bool n3ds = false;
    int setup = 0;
    (void)argc;
    (void)argv;

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, &con);
    printf("Street Fighter III 3rd Strike\n");
    APT_CheckNew3DS(&n3ds);
    if (n3ds) {
        osSetSpeedupEnable(true);
    }

    cps3_user5_stream(USER5_PAGES, USER5_RESERVE);
    if (cps3_load_rom(ROM_DIR) != 0) {
        return fail("The ROM files were not found in\n" ROM_DIR "\n\nSee the README.");
    }
    ctr_controls_load();
    if (!ctr_video_init()) {
        return fail("The GPU renderer could not start.");
    }
    ctr_prefetch_start();
    cps3_chr_dma_defer = 1;     /* big character DMA transfers run while waiting for the vertical blank */
    if (!ctr_sound_init()) {
        printf("\nNo sound: the DSP firmware is missing.\n"
               "Run DSP1 once to dump it to\nsdmc:/3ds/dspfirm.cdc\n");
        wait_key(180);
    }
    snprintf(path, sizeof(path), "%s/eeprom.bin", ROM_DIR);
    cps3_eeprom_load(path);
    pictures = ctr_bottom_init(con.frameBuffer);
    bottom_help();

    cps3_boot();
    while (aptMainLoop()) {
        hidScanInput();
        if (setup) {
            /* the game waits while the bottom screen asks for the buttons */
            if (!ctr_controls_setup_frame()) {
                setup = 0;
                bottom_help();
                ctr_bottom_redraw();
            }
            gspWaitForVBlank();
            continue;
        }
        switch (read_inputs()) {
        case QUIT:
            goto quit;
        case SETUP:
            setup = 1;
            ctr_controls_setup_begin();
            continue;
        }
        ctr_prefetch_poll();
        cps3_run_frame();
        ctr_sound_frame();
        ctr_video_render();
        ctr_bottom_frame(player_fighter());
    }
quit:
    ctr_bottom_exit();
    ctr_sound_exit();
    ctr_prefetch_stop();
    ctr_video_exit();
    gfxExit();
    return 0;
}

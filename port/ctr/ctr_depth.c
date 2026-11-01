/*
 * ctr_depth.c  3D depth for the attract screens (3DS)
 *
 * The screen is found from the game's state (G_No0 0 is the attract loop, G_No1 its stage):
 *  - boot text: in front;
 *  - CAPCOM logo: in front;
 *  - title: the background behind the screen, the logo's parts in front at three depths (by palette);
 *  - demo fights: the small logo and INSERT COIN in front.
 * Everything else is flat.
 */

#include "structs.h"
#include "work.h"
#include "ctr_depth.h"

enum { SCENE_FLAT, SCENE_BOOT, SCENE_LOGO, SCENE_TITLE, SCENE_DEMO };

static int scene;

int ctr_depth_frame(void) {
    scene = SCENE_FLAT;
    if (G_No0 == 0) {
        switch (G_No1) {
        case 0:
            scene = SCENE_BOOT;
            break;
        case 1:
            scene = SCENE_LOGO;
            break;
        case 2:
            /* the opening's last scene, or the title */
            if ((op_w.r_no_0 == 1 && op_w.r_no_1 == 18) || op_w.r_no_0 == 2) {
                scene = SCENE_TITLE;
            }
            break;
        case 3:
        case 5:
            scene = SCENE_DEMO;
            break;
        }
    }
    return scene != SCENE_FLAT;
}

s16 ctr_depth_sprite(u32 palette, int tilemap) {
    switch (scene) {
    case SCENE_LOGO:
        return 5;
    case SCENE_TITLE:
        if (tilemap) {
            return -3;
        }
        if (palette >= 0x106 && palette <= 0x10A) {
            return 5;
        }
        if (palette == 0x100 || palette == 0x101) {
            return 3;
        }
        if (palette >= 0x102 && palette <= 0x105) {
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

s16 ctr_depth_text(s32 row, u32 palette) {
    switch (scene) {
    case SCENE_BOOT:
        return 4;
    case SCENE_LOGO:
    case SCENE_TITLE:
        return 2;
    case SCENE_DEMO:
        if (palette == 29) {
            return 5;                   /* the small logo */
        }
        if (palette == 9 && row == 0) {
            return 2;                   /* INSERT COIN over the gauges */
        }
        if (palette == 9 && row >= 6 && row <= 24) {
            return 4;                   /* INSERT COIN under the logo */
        }
        return 0;
    default:
        return 0;
    }
}

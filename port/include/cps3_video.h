/*
 * cps3_video.h  Reference software renderer (cps3_video.c)
 */

#ifndef CPS3_VIDEO_H
#define CPS3_VIDEO_H

#include "types.h"

#define CPS3_VIDEO_MAX_W 512
#define CPS3_VIDEO_MAX_H 256

/* Visible size from the CRTC registers (384x224 until set). */
void cps3_video_size(s32* width, s32* height);
/* Draws a frame as 0xAARRGGBB. Pitch is in pixels. */
void cps3_video_render(u32* frame, s32 pitch);

#endif

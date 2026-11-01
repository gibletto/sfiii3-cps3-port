/*
 * ctr_video.h  The 3DS renderer: draws the CPS3 frame on the GPU
 */

#ifndef CTR_VIDEO_H
#define CTR_VIDEO_H

#include "types.h"

/* queued character DMA work done per step while waiting for the vertical blank (about 0.2 ms) */
#define CHR_IDLE_PIECE      4096

int ctr_video_init(void);       /* after gfxInit; 0 on failure */
void ctr_video_render(void);    /* draws the frame on the top screen */
void ctr_video_exit(void);

#endif

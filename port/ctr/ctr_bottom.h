/*
 * ctr_bottom.h  The bottom screen: the chosen fighter's move list (3DS)
 */

#ifndef CTR_BOTTOM_H
#define CTR_BOTTOM_H

#include "types.h"

#define CTR_BOTTOM_FIGHTERS 21      /* fighter numbers 0-20 (fighter.h) */

int ctr_bottom_init(u16* framebuffer);  /* the bottom screen's RGB565 framebuffer; 0 if no pictures */
void ctr_bottom_frame(int fighter);     /* each frame: the fighter to show, or -1 for the logo */
void ctr_bottom_exit(void);

#endif

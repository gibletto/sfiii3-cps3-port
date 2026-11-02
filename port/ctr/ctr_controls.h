/*
 * ctr_controls.h  Button mapping and the button setup screen (3DS)
 */

#ifndef CTR_CONTROLS_H
#define CTR_CONTROLS_H

#include "types.h"

#define CTR_CONTROLS_FILE "sdmc:/sf3/controls.ini"

void ctr_controls_load(void);       /* reads the file, or writes the default one if there is none */
u32 ctr_controls_map(u32 held);     /* buttons held -> the board's player inputs (attacks and START) */
int ctr_controls_region(void);      /* the file's REGION (cps3_region.h) */

void ctr_controls_setup_begin(void);
int ctr_controls_setup_frame(void); /* one frame of the setup screen on the bottom console; 0 when finished */

#endif

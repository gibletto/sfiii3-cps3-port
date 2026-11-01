/*
 * cps3_sound.h  CPS3 sound chip (cps3_sound.c)
 */

#ifndef CPS3_SOUND_H
#define CPS3_SOUND_H

#include "types.h"

/* output rate: 42.954545 MHz / 3 / 384 */
#define CPS3_SOUND_RATE (42954545.0 / 3.0 / 384.0)

void cps3_sound_reset(void);
/* Key-on register write. A bit going from 0 to 1 starts that voice. */
void cps3_sound_key_write(u32 key);
/* Renders n interleaved stereo samples. */
void cps3_sound_render(s16* out, u32 n);

#endif

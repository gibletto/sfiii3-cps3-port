/*
 * cps3_sound.h  CPS3 sound chip (cps3_sound.c)
 */

#ifndef CPS3_SOUND_H
#define CPS3_SOUND_H

#include "types.h"

/* output rate: 42.954545 MHz / 3 / 384 */
#define CPS3_SOUND_RATE (42954545.0 / 3.0 / 384.0)
/* the board's frame rate. Each game frame renders the board's samples for a frame (about 626), whatever the host's
   frame rate, because the game reads the voices' key-on bits back: its sound driver then runs as on the board. */
#define CPS3_FRAME_HZ   (42954545.0 / 6.0 / 455.0 / 264.0)

void cps3_sound_reset(void);
/* Key-on register write. A bit going from 0 to 1 starts that voice. */
void cps3_sound_key_write(u32 key);
/* Renders n interleaved stereo samples. */
void cps3_sound_render(s16* out, u32 n);

#endif

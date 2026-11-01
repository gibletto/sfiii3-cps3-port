/*
 * ctr_sound.h  Plays the sound chip's output through the 3DS DSP
 */

#ifndef CTR_SOUND_H
#define CTR_SOUND_H

int ctr_sound_init(void);       /* 0 if there is no DSP firmware: the game runs silent */
void ctr_sound_frame(void);     /* after each frame */
void ctr_sound_exit(void);

#endif

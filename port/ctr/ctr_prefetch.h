/*
 * ctr_prefetch.h  Reads the graphics ROM ahead of the game (3DS)
 */

#ifndef CTR_PREFETCH_H
#define CTR_PREFETCH_H

#include "types.h"

int ctr_prefetch_start(void);   /* after cps3_load_rom; 0 if it could not start */
void ctr_prefetch_stop(void);
void ctr_prefetch_poll(void);   /* between frames: move finished pages into the cache */

#endif

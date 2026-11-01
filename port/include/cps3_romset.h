/*
 * cps3_romset.h  Loads the game straight from MAME's ROM set zip (PC)
 */

#ifndef CPS3_ROMSET_H
#define CPS3_ROMSET_H

#include "types.h"

s32 cps3_load_romset(const char* zip_path);     /* sfiii3nr1.zip; calls cps3_load_rom_data. -1 on error */
extern char cps3_romset_error[512];             /* why it failed, for the player */

#endif

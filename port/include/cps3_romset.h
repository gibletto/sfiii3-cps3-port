/*
 * cps3_romset.h  Reads MAME's ROM set zip (sfiii3nr1.zip)
 */

#ifndef CPS3_ROMSET_H
#define CPS3_ROMSET_H

#include "types.h"

s32 cps3_load_romset(const char* zip_path);     /* PC: loads it; calls cps3_load_rom_data. -1 on error */
/* 3DS: writes bios.bin, cg.bin and user5.bin into dir, for cps3_load_rom. -1 on error */
s32 cps3_unpack_romset(const char* zip_path, const char* dir, void (*progress)(u32 done, u32 total));
extern char cps3_romset_error[512];             /* why it failed, for the player */

#endif

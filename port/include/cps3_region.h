/*
 * cps3_region.h  The region the game plays in: the BIOS's, or one chosen in the settings file
 */

#ifndef CPS3_REGION_H
#define CPS3_REGION_H

#include <stdio.h>

#include "types.h"

#define CPS3_REGION_BIOS 0      /* keep the BIOS's region */

int cps3_region_count(void);                /* BIOS, the 7 countries, and in 3S each without a CD drive */
int cps3_region_find(const char* name);     /* "USA", "asia (no cd)", "BIOS"... -> its number, -1 if unknown */
const char* cps3_region_name(int region);
const char* cps3_region_label(int region);  /* for a menu: BIOS shows the BIOS's own, "BIOS: EUROPE" */
const char* cps3_region_text(int region);   /* the language of the game's text in that region */
void cps3_region_write(FILE* f, int region); /* the settings file's REGION line, with a comment listing the names */
void cps3_region_apply(int region);         /* after the BIOS is loaded and before cps3_boot */

#endif

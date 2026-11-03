/*
 * cps3_file.h  Saving a file so that a power cut leaves the old copy or the new one, never half of one
 *
 * cps3_file_begin opens path.tmp, cps3_file_commit closes it and puts it in place of path. Call
 * cps3_file_recover before reading path: it finishes a save that was cut off between the two steps.
 * In cps3_eeprom.c.
 */

#ifndef CPS3_FILE_H
#define CPS3_FILE_H

#include <stdio.h>

FILE* cps3_file_begin(const char* path, const char* mode);
int cps3_file_commit(FILE* f, const char* path);     /* 1 if saved; closes f */
void cps3_file_recover(const char* path);

#endif

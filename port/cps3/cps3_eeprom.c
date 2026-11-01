/*
 * cps3_eeprom.c  Settings EEPROM (64 words), saved to a file
 *
 * A store at EEP_ROM + 0x80 + 2n writes word n. A load at EEP_ROM + 0x100 + 2n reads word n.
 * Other loads return the last word read, as on the board.
 */

#include <stdio.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3.h"

#define EEPROM_WORDS 64

static u16 eeprom[EEPROM_WORDS];
static u16 latch;
static char eeprom_path[512];       /* copied, the caller's buffer may be reused */

void cps3_eeprom_load(const char* path) {
    FILE* f;
    u8 b[EEPROM_WORDS * 2];
    s32 i;
    snprintf(eeprom_path, sizeof(eeprom_path), "%s", path);
    f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    if (fread(b, 1, sizeof(b), f) == sizeof(b)) {
        for (i = 0; i < EEPROM_WORDS; i++) {
            eeprom[i] = (u16)((b[i * 2] << 8) | b[i * 2 + 1]);
        }
    }
    fclose(f);
}

static void save(void) {
    FILE* f;
    u8 b[EEPROM_WORDS * 2];
    s32 i;
    if (eeprom_path[0] == 0) {
        return;
    }
    for (i = 0; i < EEPROM_WORDS; i++) {
        b[i * 2] = eeprom[i] >> 8;
        b[i * 2 + 1] = (u8)eeprom[i];
    }
    f = fopen(eeprom_path, "wb");
    if (f != NULL) {
        fwrite(b, 1, sizeof(b), f);
        fclose(f);
    }
}

s32 cps3_eeprom_read(s16 n, volatile u16* rom, u16* buf) {
    u32 ofs = (u32)rom - EEP_ROM;
    s32 i;
    for (i = 0; i < n; i++, ofs += 2) {
        if (ofs >= 0x100 && ofs < 0x180) {
            latch = eeprom[(ofs - 0x100) >> 1];
        }
        buf[i] = latch;
    }
    return 0;
}

s32 cps3_eeprom_write(s16 n, volatile u16* rom, u16* buf) {
    u32 ofs = (u32)rom - EEP_ROM;
    s32 i;
    s32 changed = 0;
    for (i = 0; i < n; i++, ofs += 2) {
        if (ofs >= 0x80 && ofs < 0x100 && eeprom[(ofs - 0x80) >> 1] != buf[i]) {
            eeprom[(ofs - 0x80) >> 1] = buf[i];
            changed = 1;
        }
    }
    /* Save only on change. An SD card write costs a 3DS a frame or more. */
    if (changed) {
        save();
    }
    return 0;
}

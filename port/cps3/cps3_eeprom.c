/*
 * cps3_eeprom.c  Settings EEPROM (64 words), saved to a file
 *
 * A store at EEP_ROM + 0x80 + 2n writes word n. A load at EEP_ROM + 0x100 + 2n reads word n.
 * Other loads return the last word read, as on the board.
 */

#include <stdio.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3_file.h"
#include "cps3.h"

#define EEPROM_WORDS 64

/* ---- saving files (cps3_file.h) ---- */

static void tmp_name(char* tmp, size_t len, const char* path) {
    snprintf(tmp, len, "%s.tmp", path);
}

FILE* cps3_file_begin(const char* path, const char* mode) {
    char tmp[600];
    tmp_name(tmp, sizeof(tmp), path);
    return fopen(tmp, mode);
}

int cps3_file_commit(FILE* f, const char* path) {
    char tmp[600];
    int ok = !ferror(f);
    tmp_name(tmp, sizeof(tmp), path);
    if (fclose(f) != 0) {
        ok = 0;
    }
    /* rename doesn't replace a file on Windows or the 3DS: remove the old one first. cps3_file_recover covers the
       moment between the two. */
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
    }
    return ok;
}

void cps3_file_recover(const char* path) {
    char tmp[600];
    FILE* f;
    tmp_name(tmp, sizeof(tmp), path);
    if ((f = fopen(path, "rb")) != NULL) {
        fclose(f);
        remove(tmp);                /* a save cut off while writing: the old file stands */
    } else {
        rename(tmp, path);          /* cut off after the old file was removed: the new one is complete */
    }
}

/* ---- the EEPROM ---- */

static u16 eeprom[EEPROM_WORDS];
static u16 latch;
static char eeprom_path[512];       /* copied, the caller's buffer may be reused */

void cps3_eeprom_load(const char* path) {
    FILE* f;
    u8 b[EEPROM_WORDS * 2];
    s32 i;
    snprintf(eeprom_path, sizeof(eeprom_path), "%s", path);
    cps3_file_recover(path);
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

static volatile int dirty;         /* changed, not saved yet (deferred saving) */
static int deferred;

/* Deferred saving: a change is only marked, and cps3_eeprom_flush (on any thread) saves it. */
void cps3_eeprom_defer(int on) {
    deferred = on;
}

int cps3_eeprom_pending(void) {
    return dirty;
}

static void save(void);

/* saves a marked change; 1 if there was one. A change made during the save marks it again. */
int cps3_eeprom_flush(void) {
    if (!dirty) {
        return 0;
    }
    dirty = 0;
    save();
    return 1;
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
    f = cps3_file_begin(eeprom_path, "wb");
    if (f != NULL) {
        fwrite(b, 1, sizeof(b), f);
        cps3_file_commit(f, eeprom_path);
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
    /* Save only on change. An SD card write costs a 3DS a frame or more, so it may be left to another thread. */
    if (changed) {
        if (deferred) {
            dirty = 1;
        } else {
            save();
        }
    }
    return 0;
}

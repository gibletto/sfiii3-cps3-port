/*
 * cps3_region.c  The region the game plays in
 *
 * The game takes its region from the BIOS, at start-up: the low half of the byte at 0x1FECB is the country (the
 * name on the title screen, the warning screen, the language of the text, the default settings), the high half
 * the version type. In 3rd Strike bit 0 of the byte at 0x1FECC says the board has no CD drive (the title screen
 * says NO CD and the test menu leaves out GAME REWRITE); 2nd Impact and New Generation don't read it, they take
 * Asia as having no CD drive. A board's region is fixed by its BIOS chip. Here a setting can change the country
 * (and with CPS3_REGION_NO_CD, 3rd Strike's build, the CD flag) in the copy of the BIOS in memory; the version
 * type and the BIOS file stay as they are.
 *
 * The text is Japanese in Japan, Spanish in Hispanic, Portuguese in Brazil and English elsewhere (Europe and
 * Oceania read a language setting too, which only 2nd Impact and New Generation's test menus show, for Europe).
 * A region with other default settings resets the operator settings (eeprom.bin) to its own, as on the board.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3_region.h"

#define REGION_BYTE 0x1FECB     /* bios_region_code */
#define CD_BYTE     0x1FECC     /* bios_cd_flags */

/* the countries the games name on the title screen, in their order; 8 (a second Asia) is left out */
static const char* const countries[8] = {
    "BIOS", "JAPAN", "ASIA", "EUROPE", "USA", "HISPANIC", "BRAZIL", "OCEANIA",
};
static const char* const texts[8] = {
    "", "Japanese", "English", "English", "English", "Spanish", "Portuguese", "English",
};

static int bios_country = -1;   /* the BIOS's own, before cps3_region_apply */
static int bios_no_cd;

#ifdef CPS3_REGION_NO_CD
/* region n > 0: country (n + 1) / 2, without a CD drive if n is even */
int cps3_region_count(void) {
    return 15;
}

static int country_of(int region) {
    return (region + 1) / 2;
}

static int no_cd_of(int region) {
    return region > 0 && region % 2 == 0;
}
#else
/* region n > 0: country n */
int cps3_region_count(void) {
    return 8;
}

static int country_of(int region) {
    return region;
}

static int no_cd_of(int region) {
    (void)region;
    return 0;
}
#endif

static void save_bios(void) {
    if (bios_country < 0) {
        bios_country = cps3_bios[REGION_BYTE] & 0xF;
#ifdef CPS3_REGION_NO_CD
        bios_no_cd = cps3_bios[CD_BYTE] & 1;
#endif
    }
}

/* upper case letters and digits only: "Asia (no CD)" -> "ASIANOCD" */
static void squeeze(char* out, const char* s, size_t size) {
    size_t n = 0;
    for (; *s != '\0' && n + 1 < size; s++) {
        if (isalnum((unsigned char)*s)) {
            out[n++] = (char)toupper((unsigned char)*s);
        }
    }
    out[n] = '\0';
}

int cps3_region_find(const char* name) {
    char want[64];
    char have[64];
    int i;
    squeeze(want, name, sizeof(want));
    if (strncmp(want, "EURO", 4) == 0 && strncmp(want, "EUROPE", 6) != 0 && strlen(want) + 2 < sizeof(want)) {
        memmove(want + 6, want + 4, strlen(want + 4) + 1);     /* EURO -> EUROPE */
        memcpy(want + 4, "PE", 2);
    }
    for (i = 0; i < cps3_region_count(); i++) {
        squeeze(have, cps3_region_name(i), sizeof(have));
        if (strcmp(want, have) == 0) {
            return i;
        }
    }
    return -1;
}

const char* cps3_region_name(int region) {
    static char name[32];
    if (region <= CPS3_REGION_BIOS || region >= cps3_region_count()) {
        return countries[CPS3_REGION_BIOS];
    }
    snprintf(name, sizeof(name), "%s%s", countries[country_of(region)], no_cd_of(region) ? " (NO CD)" : "");
    return name;
}

const char* cps3_region_label(int region) {
    static char label[40];
    int own;
    if (region > CPS3_REGION_BIOS && region < cps3_region_count()) {
        return cps3_region_name(region);
    }
    save_bios();
    own = bios_country == 8 ? 2 : bios_country;     /* 8 is a second Asia */
    if (own <= 0 || own >= 8) {
        return countries[CPS3_REGION_BIOS];
    }
    snprintf(label, sizeof(label), "BIOS: %s%s", countries[own], bios_no_cd ? " (NO CD)" : "");
    return label;
}

const char* cps3_region_text(int region) {
    int c;
    if (region > CPS3_REGION_BIOS && region < cps3_region_count()) {
        c = country_of(region);
    } else {
        save_bios();
        c = bios_country == 8 ? 2 : bios_country;
    }
    return c > 0 && c < 8 ? texts[c] : "";
}

void cps3_region_write(FILE* f, int region) {
    int i;
    fputs("; REGION: BIOS plays in the region of the ROM set's BIOS, or choose one:\n;  ", f);
    for (i = 1; i < 8; i++) {
        fprintf(f, " %s", countries[i]);
    }
#ifdef CPS3_REGION_NO_CD
    fputs("\n; each also without a CD drive, for example ASIA (NO CD): the title screen says NO CD and the test\n"
          "; menu has no GAME REWRITE.",
          f);
#endif
    fputs("\n; The text is Japanese in JAPAN, Spanish in HISPANIC, Portuguese in BRAZIL and English elsewhere.\n"
          "; A region with other default settings resets the operator (test menu) settings, as on the arcade.\n",
          f);
    fprintf(f, "REGION = %s\n", cps3_region_name(region));
}

void cps3_region_apply(int region) {
    save_bios();
    if (region > CPS3_REGION_BIOS && region < cps3_region_count()) {
        cps3_bios[REGION_BYTE] = (u8)((cps3_bios[REGION_BYTE] & 0xF0) | country_of(region));
#ifdef CPS3_REGION_NO_CD
        cps3_bios[CD_BYTE] = (u8)((cps3_bios[CD_BYTE] & ~1) | no_cd_of(region));
#endif
    }
}

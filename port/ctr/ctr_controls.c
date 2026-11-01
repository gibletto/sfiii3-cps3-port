/*
 * ctr_controls.c  Button mapping and the button setup screen (3DS)
 *
 * The mapping is kept in sdmc:/sf3/controls.ini, one line per button: "ZL = LP MP HP". The file is written with
 * the default buttons the first time the game runs. SELECT + R opens the setup screen, which asks for a button
 * for each attack, like an arcade cabinet's button check, and saves the file.
 *
 * The D-pad and Circle Pad always move, and SELECT is always the coin and menu button.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "types.h"
#include "cps3_host.h"
#include "ctr_controls.h"

#define PUNCHES     (CPS3_IN_LP | CPS3_IN_MP | CPS3_IN_HP)
#define KICKS       (CPS3_IN_LK | CPS3_IN_MK | CPS3_IN_HK)
#define NBUTTONS    9
#define NPICK       8           /* buttons the setup screen can assign: all but START */
#define START_BTN   8
#define NSTEPS      8
#define ANY_BUTTON  (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_ZL | KEY_ZR | KEY_START | KEY_SELECT)

static const struct {
    const char* name;
    u32 key;
} buttons[NBUTTONS] = {
    { "Y", KEY_Y }, { "X", KEY_X }, { "L", KEY_L },   { "B", KEY_B },         { "A", KEY_A },
    { "R", KEY_R }, { "ZL", KEY_ZL }, { "ZR", KEY_ZR }, { "START", KEY_START },
};

static const struct {
    const char* name;
    u32 in;
} inputs[] = {
    { "LP", CPS3_IN_LP }, { "MP", CPS3_IN_MP }, { "HP", CPS3_IN_HP },       { "LK", CPS3_IN_LK },
    { "MK", CPS3_IN_MK }, { "HK", CPS3_IN_HK }, { "START", CPS3_IN_START },
};

static const u32 defaults[NBUTTONS] = {
    CPS3_IN_LP, CPS3_IN_MP, CPS3_IN_HP, CPS3_IN_LK, CPS3_IN_MK, CPS3_IN_HK, PUNCHES, KICKS, CPS3_IN_START,
};

static const char header[] =
    "; Street Fighter III 3rd Strike: the 3DS buttons\n"
    ";\n"
    "; Each line is a button and what it does in the game:\n"
    ";   LP MP HP   light, medium and heavy punch\n"
    ";   LK MK HK   light, medium and heavy kick\n"
    ";   START\n"
    "; A button can do several at once: \"ZL = LP MP HP\" presses all three punches.\n"
    "; \"none\" turns a button off. The buttons are Y X L B A R ZL ZR START (ZL and ZR are on the New 3DS).\n"
    "; The D-pad and Circle Pad always move, and SELECT is always the coin and menu button.\n"
    "; In the game, hold SELECT and press R to set the buttons on screen instead.\n"
    "\n";

static u32 map[NBUTTONS];

static int find_button(const char* s) {
    int i;
    for (i = 0; i < NBUTTONS; i++) {
        if (strcasecmp(s, buttons[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

static u32 find_input(const char* s) {
    u32 i;
    for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        if (strcasecmp(s, inputs[i].name) == 0) {
            return inputs[i].in;
        }
    }
    return 0;       /* "none" and anything unknown */
}

/* reads "button = inputs" lines; the buttons a file leaves out do nothing. 0 if it has none */
static int parse(FILE* f) {
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char* eq;
        char* tok;
        int b;
        u32 in = 0;
        line[strcspn(line, ";#\r\n")] = '\0';
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        tok = strtok(line, " \t");
        if (tok == NULL || (b = find_button(tok)) < 0) {
            continue;
        }
        for (tok = strtok(eq + 1, " \t+,"); tok != NULL; tok = strtok(NULL, " \t+,")) {
            in |= find_input(tok);
        }
        if (!found) {
            memset(map, 0, sizeof(map));
            found = 1;
        }
        map[b] = in;
    }
    return found;
}

static int save(void) {
    FILE* f = fopen(CTR_CONTROLS_FILE, "w");
    int b;
    u32 i;
    if (f == NULL) {
        return 0;
    }
    fputs(header, f);
    for (b = 0; b < NBUTTONS; b++) {
        fprintf(f, "%s =", buttons[b].name);
        if (map[b] == 0) {
            fputs(" none", f);
        }
        for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
            if (map[b] & inputs[i].in) {
                fprintf(f, " %s", inputs[i].name);
            }
        }
        fputc('\n', f);
    }
    return fclose(f) == 0;
}

void ctr_controls_load(void) {
    FILE* f = fopen(CTR_CONTROLS_FILE, "r");
    memcpy(map, defaults, sizeof(map));
    if (f == NULL) {
        save();
        return;
    }
    if (!parse(f)) {
        memcpy(map, defaults, sizeof(map));
    }
    fclose(f);
}

u32 ctr_controls_map(u32 held) {
    u32 in = 0;
    int b;
    for (b = 0; b < NBUTTONS; b++) {
        if (held & buttons[b].key) {
            in |= map[b];
        }
    }
    return in;
}

/* the setup screen */

static const struct {
    const char* label;
    u32 in;
} steps[NSTEPS] = {
    { "Light punch", CPS3_IN_LP }, { "Medium punch", CPS3_IN_MP }, { "Heavy punch", CPS3_IN_HP },
    { "Light kick", CPS3_IN_LK },  { "Medium kick", CPS3_IN_MK },  { "Heavy kick", CPS3_IN_HK },
    { "All three punches", PUNCHES }, { "All three kicks", KICKS },
};

enum { RELEASE, ASK, DONE };

static int phase;
static int step;                /* the attack being asked for */
static int pick[NSTEPS];        /* the button chosen for each, or -1 for none */
static int frames;
static const char* result;

static void draw(void) {
    int i;
    printf("\x1b[2J\x1b[2;2HBUTTON SETUP\n\n");
    printf(" Press the button for each attack.\n The game waits until you have finished.\n\n");
    for (i = 0; i < NSTEPS; i++) {
        const char* name = "";
        if (i < step) {
            name = pick[i] < 0 ? "-" : buttons[pick[i]].name;
        }
        if (i == step && phase != DONE) {
            printf("\x1b[33m > %-20s?\x1b[0m\n", steps[i].label);
        } else {
            printf("   %-20s%s\n", steps[i].label, name);
        }
    }
    if (phase == DONE) {
        printf("\n %s\n", result);
    } else {
        printf("\n SELECT  no button for this one\n START   cancel, keep the old buttons\n");
    }
}

void ctr_controls_setup_begin(void) {
    int i;
    for (i = 0; i < NSTEPS; i++) {
        pick[i] = -1;
    }
    phase = RELEASE;
    step = 0;
    draw();
}

static void finish(const char* message) {
    result = message;
    phase = DONE;
    frames = 0;
    draw();
}

int ctr_controls_setup_frame(void) {
    u32 held = hidKeysHeld();
    u32 down = hidKeysDown();
    int i, b;
    switch (phase) {
    case RELEASE:       /* the buttons that opened the screen are still held */
        if ((held & ANY_BUTTON) == 0) {
            phase = ASK;
        }
        return 1;
    case ASK:
        if (down & KEY_START) {
            finish("Cancelled: the buttons are unchanged.");
            return 1;
        }
        if (down & KEY_SELECT) {
            pick[step++] = -1;
        } else {
            for (b = 0; b < NPICK; b++) {
                if (down & buttons[b].key) {
                    for (i = 0; i < step && pick[i] != b; i++) {
                    }
                    if (i == step) {
                        break;      /* not used yet */
                    }
                }
            }
            if (b == NPICK) {
                return 1;
            }
            pick[step++] = b;
        }
        if (step < NSTEPS) {
            draw();
            return 1;
        }
        for (b = 0; b < NPICK; b++) {
            map[b] = 0;     /* START keeps what it had */
        }
        for (i = 0; i < NSTEPS; i++) {
            if (pick[i] >= 0) {
                map[pick[i]] |= steps[i].in;
            }
        }
        finish(save() ? "Saved to " CTR_CONTROLS_FILE
                      : "Could not save " CTR_CONTROLS_FILE ",\n but the buttons are set until you quit.");
        return 1;
    default:            /* show the result, then wait for the buttons to be let go */
        return ++frames < 90 || (held & ANY_BUTTON) != 0;
    }
}

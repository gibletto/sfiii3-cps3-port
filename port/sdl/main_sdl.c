/*
 * main_sdl.c  The PC front end (SDL2)
 *
 *     sf3 [sfiii3nr1.zip | folder]
 *
 * The ROM set is MAME's sfiii3nr1.zip, read as it is (cps3_romset.c). With no argument it is looked for in the
 * roms folder next to sf3.exe, then in rom-data folders (bios.bin, cg.bin and user5.bin, as the 3DS unpacks
 * them). The settings EEPROM is saved next to the ROM set, with settings.ini (the region, cps3_region.c, and the
 * picture's shape and size, SCREEN and SCALE).
 *
 * Runs the board at its 59.59 Hz frame rate with the software renderer. Alt+Enter or F11 toggles full screen,
 * F12 saves frame.bmp, Escape quits.
 *
 * Controls (MAME's defaults where it has one): arrows, A S D punches, Z X C kicks, 1 start, 5 coin; 2 and 6 for
 * player 2's start and coin; F2 the test switch (settings, free play), 9 the service coin. Game controllers:
 * the first is player 1, the second player 2; D-pad or left stick, X Y RB punches, A B RT kicks, Start, Back
 * for a coin.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3_kernel.h"
#include "cps3_video.h"
#include "cps3_sound.h"
#include "cps3_region.h"
#include "cps3_romset.h"

#define FRAME_HZ (42954545.0 / 6.0 / 455.0 / 264.0)

static u32 frame[CPS3_VIDEO_MAX_H][CPS3_VIDEO_MAX_W];

void host_divzero_install(void);

static SDL_GameController* pads[2];

#define TRIGGER_ON  1000    /* of 32767: any press of the trigger, above the noise some pads have at rest */

static u32 pad_input(SDL_GameController* c) {
    static const struct {
        SDL_GameControllerButton b;
        u32 in;
    } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP, CPS3_IN_UP },       { SDL_CONTROLLER_BUTTON_DPAD_DOWN, CPS3_IN_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, CPS3_IN_LEFT },   { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, CPS3_IN_RIGHT },
        { SDL_CONTROLLER_BUTTON_X, CPS3_IN_LP },             { SDL_CONTROLLER_BUTTON_Y, CPS3_IN_MP },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, CPS3_IN_HP }, { SDL_CONTROLLER_BUTTON_A, CPS3_IN_LK },
        { SDL_CONTROLLER_BUTTON_B, CPS3_IN_MK },             { SDL_CONTROLLER_BUTTON_START, CPS3_IN_START },
        { SDL_CONTROLLER_BUTTON_BACK, CPS3_IN_COIN },
    };
    u32 in = 0;
    u32 i;
    s32 x, y;
    if (c == NULL) {
        return 0;
    }
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (SDL_GameControllerGetButton(c, map[i].b)) {
            in |= map[i].in;
        }
    }
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_ON) {
        in |= CPS3_IN_HK;
    }
    x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    in |= x < -16000 ? CPS3_IN_LEFT : x > 16000 ? CPS3_IN_RIGHT : 0;
    in |= y < -16000 ? CPS3_IN_UP : y > 16000 ? CPS3_IN_DOWN : 0;
    return in;
}

static void open_pads(void) {
    s32 i, n = 0;
    for (i = 0; i < 2; i++) {
        if (pads[i] != NULL && !SDL_GameControllerGetAttached(pads[i])) {
            SDL_GameControllerClose(pads[i]);
            pads[i] = NULL;
        }
    }
    for (i = 0; i < SDL_NumJoysticks() && n < 2; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* c = SDL_GameControllerOpen(i);     /* returns the same one if already open */
            if (pads[0] != c && pads[1] != c) {
                if (pads[0] == NULL) {
                    pads[0] = c;
                } else if (pads[1] == NULL) {
                    pads[1] = c;
                }
            } else {
                SDL_GameControllerClose(c);                         /* drop the extra reference */
            }
            n++;
        }
    }
}

static void read_inputs(void) {
    static const struct {
        SDL_Scancode k;
        u32 in;
    } p1_keys[] = {
        { SDL_SCANCODE_UP, CPS3_IN_UP }, { SDL_SCANCODE_DOWN, CPS3_IN_DOWN }, { SDL_SCANCODE_LEFT, CPS3_IN_LEFT },
        { SDL_SCANCODE_RIGHT, CPS3_IN_RIGHT }, { SDL_SCANCODE_A, CPS3_IN_LP }, { SDL_SCANCODE_S, CPS3_IN_MP },
        { SDL_SCANCODE_D, CPS3_IN_HP }, { SDL_SCANCODE_Z, CPS3_IN_LK }, { SDL_SCANCODE_X, CPS3_IN_MK },
        { SDL_SCANCODE_C, CPS3_IN_HK }, { SDL_SCANCODE_1, CPS3_IN_START }, { SDL_SCANCODE_5, CPS3_IN_COIN },
    };
    const Uint8* k = SDL_GetKeyboardState(NULL);
    u32 p1 = pad_input(pads[0]);
    u32 p2 = pad_input(pads[1]);
    u32 sys = 0;
    u32 i;
    for (i = 0; i < sizeof(p1_keys) / sizeof(p1_keys[0]); i++) {
        if (k[p1_keys[i].k]) {
            p1 |= p1_keys[i].in;
        }
    }
    p2 |= k[SDL_SCANCODE_2] ? CPS3_IN_START : 0;
    p2 |= k[SDL_SCANCODE_6] ? CPS3_IN_COIN : 0;
    sys |= k[SDL_SCANCODE_F2] ? CPS3_SYS_TEST : 0;
    sys |= k[SDL_SCANCODE_9] ? CPS3_SYS_SERVICE : 0;
    cps3_set_inputs(p1, p2, sys);
}

static void save_bmp(const char* path, s32 w, s32 h) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(frame, w, h, 32, CPS3_VIDEO_MAX_W * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s != NULL) {
        SDL_SaveBMP(s, path);
        SDL_FreeSurface(s);
    }
}

/* ---- sound: each frame's samples are queued to the audio device ----
 *
 * The device is opened at its own rate and the sound chip's 37287 Hz is converted here (SDL_AudioStream).
 * Asking WASAPI to convert the odd rate itself made it drop about 8 ms of sound every second or so.
 */

static SDL_AudioDeviceID audio_dev;
static SDL_AudioStream* audio_conv;
static double audio_ratio;          /* device samples per sound chip sample */
static double sound_acc;
static s16 sound_buf[2 * 2048];
static s16 conv_buf[2 * 4096];

static void sound_queue(const s16* buf, u32 n) {
    int got;
    SDL_AudioStreamPut(audio_conv, buf, (int)(n * 4));
    while ((got = SDL_AudioStreamGet(audio_conv, conv_buf, sizeof(conv_buf))) > 0) {
        SDL_QueueAudio(audio_dev, conv_buf, (u32)got);
    }
}

static void sound_open(void) {
    SDL_AudioSpec want, have;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        return;
    }
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (audio_dev != 0) {
        audio_conv = SDL_NewAudioStream(AUDIO_S16SYS, 2, (int)(CPS3_SOUND_RATE + 0.5), AUDIO_S16SYS, 2, have.freq);
        if (audio_conv == NULL) {
            SDL_CloseAudioDevice(audio_dev);
            audio_dev = 0;
        }
    }
    if (audio_dev == 0) {
        fprintf(stderr, "no sound: %s\n", SDL_GetError());
    } else {
        u32 n = (u32)(CPS3_SOUND_RATE / FRAME_HZ) * 3;
        audio_ratio = have.freq / CPS3_SOUND_RATE;
        memset(sound_buf, 0, sizeof(sound_buf));
        sound_queue(sound_buf, n);      /* three frames ahead */
        SDL_PauseAudioDevice(audio_dev, 0);
    }
}

/* stretches n stereo samples to m (linear) into stretch_buf */
static s16 stretch_buf[2 * 2200];

static void stretch(const s16* in, u32 n, u32 m) {
    u32 i;
    for (i = 0; i < m; i++) {
        u32 at = (u32)(((Uint64)i * (n - 1) << 16) / (m > 1 ? m - 1 : 1));
        u32 k = at >> 16, f = at & 0xFFFF;
        u32 k1 = k + 1 < n ? k + 1 : k;
        stretch_buf[i * 2] = (s16)(in[k * 2] + (((s32)in[k1 * 2] - in[k * 2]) * (s32)f >> 16));
        stretch_buf[i * 2 + 1] = (s16)(in[k * 2 + 1] + (((s32)in[k1 * 2 + 1] - in[k * 2 + 1]) * (s32)f >> 16));
    }
}

/* Each frame renders the board's samples for a frame: the game reads the voices' state back, so the count must
   not depend on the sound device. The device's clock drifts from the frame rate, so to keep 2-6 frames queued
   the samples are stretched a little (under 1%) on the way out, or a frame of silence goes ahead after a stall. */
static void sound_frame(void) {
    u32 n;
    sound_acc += CPS3_SOUND_RATE / CPS3_FRAME_HZ;
    n = (u32)sound_acc;
    sound_acc -= n;
    cps3_sound_render(sound_buf, n);
    if (audio_dev != 0) {
        u32 queued = (u32)(SDL_GetQueuedAudioSize(audio_dev) / 4 / audio_ratio);
        u32 per = (u32)(CPS3_SOUND_RATE / CPS3_FRAME_HZ);
        if (queued < per) {
            memset(stretch_buf, 0, per * 4);
            sound_queue(stretch_buf, per);
        }
        if (queued > per * 6) {
            stretch(sound_buf, n, n - n / 128);
            sound_queue(stretch_buf, n - n / 128);
        } else if (queued < per * 2) {
            stretch(sound_buf, n, n + n / 128);
            sound_queue(stretch_buf, n + n / 128);
        } else {
            sound_queue(sound_buf, n);
        }
    }
}

#define TITLE "Street Fighter III 3rd Strike"

static int is_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
    }
    return f != NULL;
}

static int ends_with(const char* s, const char* end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && SDL_strcasecmp(s + n - m, end) == 0;
}

/* loads the ROM set from the argument or the usual places; save_dir gets the folder for the EEPROM */
static int load_game(const char* arg, char* save_dir, size_t len) {
    char* base = SDL_GetBasePath();     /* sf3.exe's folder, with a trailing slash */
    char zip[512], dir[512], msg[1200];
    int found = 0;
    zip[0] = dir[0] = '\0';
    if (arg != NULL && ends_with(arg, ".zip")) {
        snprintf(zip, sizeof(zip), "%s", arg);
    } else if (arg != NULL) {
        snprintf(zip, sizeof(zip), "%s/sfiii3nr1.zip", arg);
        if (!is_file(zip)) {
            zip[0] = '\0';
            snprintf(dir, sizeof(dir), "%s", arg);
        }
    } else {
        const char* tries[3][2] = { { base, "roms" }, { base, "rom-data" }, { "", "rom-data" } };
        int i;
        for (i = 0; i < 3 && !found; i++) {
            snprintf(zip, sizeof(zip), "%s%s/sfiii3nr1.zip", tries[i][0] ? tries[i][0] : "", tries[i][1]);
            snprintf(dir, sizeof(dir), "%s%s", tries[i][0] ? tries[i][0] : "", tries[i][1]);
            if (is_file(zip)) {
                dir[0] = '\0';
                found = 1;
            } else {
                char bios[600];
                zip[0] = '\0';
                snprintf(bios, sizeof(bios), "%s/bios.bin", dir);
                found = is_file(bios);
            }
        }
        if (!found) {
            snprintf(msg, sizeof(msg), "The ROM set was not found.\n\nPut sfiii3nr1.zip (MAME, non-merged) in\n%sroms",
                     base ? base : "");
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, msg, NULL);
            SDL_free(base);
            return 0;
        }
    }
    SDL_free(base);
    if (zip[0] != '\0') {
        const char* slash = strrchr(zip, '/');
        if (strrchr(zip, '\\') > slash) {
            slash = strrchr(zip, '\\');
        }
        snprintf(save_dir, len, "%.*s", slash != NULL ? (int)(slash - zip) : 1, slash != NULL ? zip : ".");
        if (cps3_load_romset(zip) != 0) {
            snprintf(msg, sizeof(msg), "Could not load the ROM set:\n%s\n\n%s", zip, cps3_romset_error);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, msg, NULL);
            return 0;
        }
    } else {
        snprintf(save_dir, len, "%s", dir);
        if (cps3_load_rom(dir) != 0) {
            snprintf(msg, sizeof(msg), "Could not load the ROM files (bios.bin, cg.bin, user5.bin) from\n%s", dir);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, msg, NULL);
            return 0;
        }
    }
    return 1;
}

/* ---- the picture: its shape (settings.ini SCREEN) and size in the window (SCALE) ----
 *
 * SQUARE shows the game's pixels square. 4:3 gives them the shape a 4:3 arcade monitor did: 384x224 filled its
 * screen, so a pixel is 7/9 as wide as it is tall.
 *   INTEGER  the height a whole multiple of the game's, as big as fits: lines stay sharp
 *   PERFECT  whole multiples both ways, so every pixel is the same; for 4:3 the nearest whole-number shape
 *            within 5% (3x4 at 1080p, 4x5 at 1440p, exactly 7x9 at 4K), or as INTEGER if none fits
 *   FIT      as big as the window allows
 * A size that isn't a whole multiple is drawn "sharp bilinear": scaled up by whole numbers (nearest) into
 * `big`, then smoothed to the size, so only the pixels' edges blend.
 */

enum { SHAPE_SQUARE, SHAPE_43 };
enum { SCALE_INTEGER, SCALE_PERFECT, SCALE_FIT };

static const char* const shape_names[] = { "SQUARE", "4:3" };
static const char* const scale_names[] = { "INTEGER", "PERFECT", "FIT" };
static int shape = SHAPE_SQUARE;
static int scale_mode = SCALE_INTEGER;

/* the width of a w-pixel line n lines tall */
static s32 shaped_width(s32 w, s32 n) {
    return shape == SHAPE_43 ? (w * n * 7 + 4) / 9 : w * n;
}

/* where a w x h frame goes in an ow x oh window */
static void place(s32 w, s32 h, s32 ow, s32 oh, SDL_Rect* r) {
    s32 n, b;
    r->w = r->h = 0;
    if (scale_mode == SCALE_PERFECT && shape == SHAPE_43) {
        for (b = oh / h; b >= 1 && r->w == 0; b--) {
            s32 a = (b * 7 + 4) / 9;
            double err = (double)a * 9 / ((double)b * 7) - 1.0;
            if (a >= 1 && a * w <= ow && err <= 0.05 && err >= -0.05) {
                r->w = a * w;
                r->h = b * h;
            }
        }
    }
    if (r->w == 0 && scale_mode != SCALE_FIT) {
        for (n = oh / h; n >= 1 && shaped_width(w, n) > ow; n--) {
        }
        if (n >= 1) {
            r->w = shaped_width(w, n);
            r->h = n * h;
        }
    }
    if (r->w == 0) {        /* FIT, or a window smaller than the game */
        double aspect = (double)w * (shape == SHAPE_43 ? 7.0 / 9.0 : 1.0) / h;
        r->h = oh;
        r->w = (s32)(oh * aspect + 0.5);
        if (r->w > ow) {
            r->w = ow;
            r->h = (s32)(ow / aspect + 0.5);
        }
    }
    r->x = (ow - r->w) / 2;
    r->y = (oh - r->h) / 2;
}

static SDL_Texture* big;
static s32 big_w, big_h;

/* draws the w x h frame in `tex` into the window */
static void draw_picture(SDL_Renderer* ren, SDL_Texture* tex, s32 w, s32 h) {
    SDL_Rect src, dst;
    s32 ow, oh, px, py;
    src.x = 0;
    src.y = 0;
    src.w = w;
    src.h = h;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    place(w, h, ow, oh, &dst);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (dst.w % w == 0 && dst.h % h == 0) {
        SDL_RenderCopy(ren, tex, &src, &dst);       /* whole multiples: nearest is exact */
        return;
    }
    px = (dst.w + w - 1) / w;
    py = (dst.h + h - 1) / h;
    if (big == NULL || big_w != w * px || big_h != h * py) {
        if (big != NULL) {
            SDL_DestroyTexture(big);
        }
        big = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w * px, h * py);
        big_w = w * px;
        big_h = h * py;
        if (big != NULL) {
            SDL_SetTextureScaleMode(big, SDL_ScaleModeLinear);
        }
    }
    if (big == NULL || SDL_SetRenderTarget(ren, big) != 0) {
        SDL_RenderCopy(ren, tex, &src, &dst);       /* no render targets: plain nearest */
        return;
    }
    SDL_RenderCopy(ren, tex, &src, NULL);
    SDL_SetRenderTarget(ren, NULL);
    SDL_RenderCopy(ren, big, NULL, &dst);
}

/* settings.ini, next to the EEPROM: the region, SCREEN and SCALE. The lines it lacks are added, as the 3DS adds
   them to its controls.ini */
static void load_settings(const char* dir) {
    char path[600];
    char line[256];
    int region = CPS3_REGION_BIOS;
    int found = 0, have_screen = 0, have_scale = 0, full = 0;
    FILE* f;
    snprintf(path, sizeof(path), "%s/settings.ini", dir);
    if ((f = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            char* eq;
            char* key;
            line[strcspn(line, ";#\r\n")] = '\0';
            if ((eq = strchr(line, '=')) == NULL) {
                continue;
            }
            *eq = '\0';
            key = strtok(line, " \t");
            if (key != NULL && strcasecmp(key, "REGION") == 0) {
                region = cps3_region_find(eq + 1) >= 0 ? cps3_region_find(eq + 1) : CPS3_REGION_BIOS;
                found = 1;
            } else if (key != NULL && strcasecmp(key, "SCREEN") == 0) {
                char* v = strtok(eq + 1, " \t");
                have_screen = 1;
                shape = v != NULL && strcmp(v, "4:3") == 0 ? SHAPE_43 : SHAPE_SQUARE;
                v = strtok(NULL, " \t");
                full = shape == SHAPE_43 && v != NULL && strcasecmp(v, "FULL") == 0;   /* the 3DS's: as FIT */
            } else if (key != NULL && strcasecmp(key, "SCALE") == 0) {
                char* v = strtok(eq + 1, " \t");
                have_scale = 1;
                scale_mode = SCALE_INTEGER;
                if (v != NULL && strcasecmp(v, "PERFECT") == 0) {
                    scale_mode = SCALE_PERFECT;
                } else if (v != NULL && strcasecmp(v, "FIT") == 0) {
                    scale_mode = SCALE_FIT;
                }
            }
        }
        fclose(f);
    }
    if (full && !have_scale) {
        scale_mode = SCALE_FIT;
    }
    if ((!found || !have_screen || !have_scale) && (f = fopen(path, "a")) != NULL) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            fputs("; " TITLE ": settings\n\n", f);
        }
        if (!found) {
            cps3_region_write(f, region);
        }
        if (!have_screen || !have_scale) {
            fputs("\n; SCREEN: SQUARE shows the game's pixels square. 4:3 gives them the shape a 4:3 arcade monitor did,\n"
                  "; so the picture is narrower.\n"
                  "; SCALE: INTEGER makes the height a whole multiple of the game's, so lines stay sharp. PERFECT makes\n"
                  "; every pixel the same size (with 4:3, a shape within 5% of it; exact on a 4K screen). FIT fills\n"
                  "; the window.\n",
                  f);
        }
        if (!have_screen) {
            fprintf(f, "SCREEN = %s\n", shape_names[shape]);
        }
        if (!have_scale) {
            fprintf(f, "SCALE = %s\n", scale_names[scale_mode]);
        }
        fclose(f);
    }
    cps3_region_apply(region);
}

static void toggle_fullscreen(SDL_Window* win) {
    int full = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(win, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowCursor(full ? SDL_ENABLE : SDL_DISABLE);
}

int main(int argc, char** argv) {
    char dir[512];
    char path[600];
    SDL_Window* win;
    SDL_Renderer* ren;
    SDL_Texture* tex;
    Uint64 freq, next;
    s32 w = 384, h = 224;
    s32 running = 1;
    s32 n;
    SDL_DisplayMode desk;

    host_divzero_install();
    if (!load_game(argc > 1 ? argv[1] : NULL, dir, sizeof(dir))) {
        return 1;
    }
    load_settings(dir);
    snprintf(path, sizeof(path), "%s/eeprom.bin", dir);
    cps3_eeprom_load(path);

    /* the window in real pixels: with Windows' display scaling stretching it, no size would be a whole multiple */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /* the window starts a whole multiple of the game's height, about 3/4 of the screen's */
    n = SDL_GetDesktopDisplayMode(0, &desk) == 0 ? desk.h * 3 / 4 / 224 : 3;
    if (n < 1) {
        n = 1;
    }
    win = SDL_CreateWindow(TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, shaped_width(384, n), 224 * n,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (ren == NULL) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    }
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, CPS3_VIDEO_MAX_W,
                            CPS3_VIDEO_MAX_H);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);

    open_pads();
    sound_open();
    cps3_boot();
    freq = SDL_GetPerformanceFrequency();
    next = SDL_GetPerformanceCounter();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) {
                save_bmp("frame.bmp", w, h);
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat &&
                       (e.key.keysym.sym == SDLK_F11 ||
                        (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT) != 0))) {
                toggle_fullscreen(win);
            } else if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) {
                open_pads();
            }
        }
        read_inputs();
        cps3_run_frame();
        sound_frame();
        cps3_video_render(&frame[0][0], CPS3_VIDEO_MAX_W);
        cps3_video_size(&w, &h);
        SDL_UpdateTexture(tex, NULL, frame, CPS3_VIDEO_MAX_W * 4);
        draw_picture(ren, tex, w, h);
        SDL_RenderPresent(ren);

        next += (Uint64)(freq / FRAME_HZ);
        {
            Uint64 now = SDL_GetPerformanceCounter();
            if (now < next) {
                SDL_Delay((u32)((next - now) * 1000 / freq));
            } else if (now > next + freq / 4) {
                next = now;     /* far behind: don't try to catch up */
            }
        }
    }
    if (audio_dev != 0) {
        SDL_CloseAudioDevice(audio_dev);
        SDL_FreeAudioStream(audio_conv);
    }
    if (big != NULL) {
        SDL_DestroyTexture(big);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

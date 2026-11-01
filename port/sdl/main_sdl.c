/*
 * main_sdl.c  The PC front end (SDL2)
 *
 *     sf3 [rom-data dir]       (default: rom-data, made by port/tools/prepare_rom.py)
 *
 * Runs the board at its 59.59 Hz frame rate with the software renderer. F12 saves frame.bmp; Escape quits.
 *
 * Controls (MAME's defaults where it has one): arrows, A S D punches, Z X C kicks, 1 start, 5 coin; 2 and 6 for
 * player 2's start and coin; F2 the test switch (settings, free play), 9 the service coin. Game controllers:
 * the first is player 1, the second player 2; D-pad or left stick, X Y RB punches, A B RT kicks, Start, Back
 * for a coin.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3_kernel.h"
#include "cps3_video.h"
#include "cps3_sound.h"

#define SCALE 3
#define FRAME_HZ (42954545.0 / 6.0 / 455.0 / 264.0)

static u32 frame[CPS3_VIDEO_MAX_H][CPS3_VIDEO_MAX_W];

void host_divzero_install(void);

static SDL_GameController* pads[2];

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
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) {
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

/* ---- sound: each frame's samples are queued to the audio device ---- */

static SDL_AudioDeviceID audio_dev;
static double sound_acc;
static s16 sound_buf[2 * 2048];

static void sound_open(void) {
    SDL_AudioSpec want, have;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        return;
    }
    SDL_zero(want);
    want.freq = (int)(CPS3_SOUND_RATE + 0.5);
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);     /* SDL converts to the device's rate */
    if (audio_dev == 0) {
        fprintf(stderr, "no sound: %s\n", SDL_GetError());
    } else {
        u32 n = (u32)(CPS3_SOUND_RATE / FRAME_HZ) * 3;
        memset(sound_buf, 0, sizeof(sound_buf));
        SDL_QueueAudio(audio_dev, sound_buf, n * 4);   /* three frames ahead */
        SDL_PauseAudioDevice(audio_dev, 0);
    }
}

static void sound_frame(void) {
    u32 n;
    sound_acc += CPS3_SOUND_RATE / FRAME_HZ;
    n = (u32)sound_acc;
    sound_acc -= n;
    if (audio_dev != 0) {
        /* the device's clock drifts from the frame rate: adjust a little to keep 2-6 frames queued */
        u32 queued = SDL_GetQueuedAudioSize(audio_dev) / 4, per = (u32)(CPS3_SOUND_RATE / FRAME_HZ);
        if (queued > per * 6) {
            n -= n / 32;
        } else if (queued < per * 2) {
            n += n / 32;
        }
    }
    cps3_sound_render(sound_buf, n);
    if (audio_dev != 0) {
        SDL_QueueAudio(audio_dev, sound_buf, n * 4);
    }
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "rom-data";
    char path[512];
    SDL_Window* win;
    SDL_Renderer* ren;
    SDL_Texture* tex;
    Uint64 freq, next;
    s32 w = 384, h = 224;
    s32 running = 1;

    host_divzero_install();
    if (cps3_load_rom(dir) != 0) {
        return 1;
    }
    snprintf(path, sizeof(path), "%s/eeprom.bin", dir);
    cps3_eeprom_load(path);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("Street Fighter III 3rd Strike", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           384 * SCALE, 224 * SCALE, SDL_WINDOW_RESIZABLE);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, CPS3_VIDEO_MAX_W,
                            CPS3_VIDEO_MAX_H);
    SDL_RenderSetLogicalSize(ren, 384, 224);

    open_pads();
    sound_open();
    cps3_boot();
    freq = SDL_GetPerformanceFrequency();
    next = SDL_GetPerformanceCounter();
    while (running) {
        SDL_Event e;
        SDL_Rect src;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) {
                save_bmp("frame.bmp", w, h);
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
        src.x = 0;
        src.y = 0;
        src.w = w;
        src.h = h;
        SDL_RenderSetLogicalSize(ren, w, h);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, &src, NULL);
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
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

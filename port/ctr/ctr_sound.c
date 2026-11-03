/*
 * ctr_sound.c  Plays the sound chip's output through the 3DS DSP
 *
 * Each frame renders the board's samples for a frame (the game reads the voices' state back, so the count must not
 * depend on the 3DS), queued as one buffer. The screens refresh at 59.83 Hz against the board's 59.60, so the DSP
 * plays them that much faster, and a little faster or slower again to keep a few buffers queued as its clock
 * drifts from the screens'. Needs the DSP firmware at sdmc:/3ds/dspfirm.cdc.
 */

#include <string.h>

#include <3ds.h>

#include "types.h"
#include "cps3_sound.h"
#include "ctr_sound.h"

#define CHANNEL     0
#define NBUF        8
#define BUF_SAMPLES 2048        /* a frame is about 626 */
#define FRAME_HZ    (268111856.0 / 4481136.0)   /* the screens' refresh rate */
#define RATE_NUDGE  0.0025                      /* 4 cents: enough for clock drift, too little to hear */

static ndspWaveBuf wbuf[NBUF];
static s16* data;
static s16 scratch[BUF_SAMPLES * 2];
static int ready;
static u32 next;
static double acc;

static void set_rate(int nudge);

static int busy(const ndspWaveBuf* b) {
    return b->status == NDSP_WBUF_QUEUED || b->status == NDSP_WBUF_PLAYING;
}

int ctr_sound_init(void) {
    float mix[12];
    u32 i;
    if (R_FAILED(ndspInit())) {
        return 0;
    }
    data = linearAlloc(NBUF * BUF_SAMPLES * 4);
    if (data == NULL) {
        ndspExit();
        return 0;
    }
    memset(data, 0, NBUF * BUF_SAMPLES * 4);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(CHANNEL);
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
    set_rate(0);
    ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(CHANNEL, mix);
    memset(wbuf, 0, sizeof(wbuf));
    for (i = 0; i < NBUF; i++) {
        wbuf[i].data_pcm16 = data + i * BUF_SAMPLES * 2;
        wbuf[i].status = NDSP_WBUF_DONE;
    }
    /* two frames of silence ahead */
    DSP_FlushDataCache(data, NBUF * BUF_SAMPLES * 4);
    for (i = 0; i < 2; i++) {
        wbuf[i].nsamples = (u32)(CPS3_SOUND_RATE / CPS3_FRAME_HZ);
        ndspChnWaveBufAdd(CHANNEL, &wbuf[i]);
    }
    next = 2;
    ready = 1;
    return 1;
}

/* the DSP plays a board frame's samples per screen frame, nudged by `nudge` to keep 2-3 buffers queued */
static void set_rate(int nudge) {
    static int now = 99;
    if (nudge != now) {
        now = nudge;
        ndspChnSetRate(CHANNEL, (float)(CPS3_SOUND_RATE * FRAME_HZ / CPS3_FRAME_HZ * (1.0 + nudge * RATE_NUDGE)));
    }
}

void ctr_sound_frame(void) {
    u32 n, queued = 0, i;
    ndspWaveBuf* b;
    acc += CPS3_SOUND_RATE / CPS3_FRAME_HZ;
    n = (u32)acc;
    acc -= n;
    if (!ready) {
        cps3_sound_render(scratch, n);      /* no DSP: still run the voices, the game reads their state */
        return;
    }
    for (i = 0; i < NBUF; i++) {
        queued += busy(&wbuf[i]);
    }
    set_rate(queued <= 1 ? -1 : queued >= 4 ? 1 : 0);
    b = &wbuf[next];
    if (queued == 0 && !busy(b)) {
        /* ran dry after a late frame: a frame of silence ahead again */
        memset(b->data_pcm16, 0, n * 4);
        b->nsamples = n;
        DSP_FlushDataCache(b->data_pcm16, n * 4);
        ndspChnWaveBufAdd(CHANNEL, b);
        next = (next + 1) % NBUF;
        b = &wbuf[next];
    }
    if (busy(b)) {
        cps3_sound_render(scratch, n);      /* all buffers busy: drop this frame */
        return;
    }
    cps3_sound_render(b->data_pcm16, n);
    b->nsamples = n;
    DSP_FlushDataCache(b->data_pcm16, n * 4);
    ndspChnWaveBufAdd(CHANNEL, b);
    next = (next + 1) % NBUF;
}

void ctr_sound_exit(void) {
    if (ready) {
        ndspChnWaveBufClear(CHANNEL);
        ndspExit();
        linearFree(data);
        ready = 0;
    }
}

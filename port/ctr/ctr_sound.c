/*
 * ctr_sound.c  Plays the sound chip's output through the 3DS DSP
 *
 * Each frame's samples are queued as one buffer. The DSP's clock drifts from the frame rate, so each buffer is
 * made a little longer or shorter to keep a few queued. Needs the DSP firmware at sdmc:/3ds/dspfirm.cdc.
 */

#include <string.h>

#include <3ds.h>

#include "types.h"
#include "cps3_sound.h"
#include "ctr_sound.h"

#define CHANNEL     0
#define NBUF        8
#define BUF_SAMPLES 2048        /* a frame is about 623 */
#define FRAME_HZ    (268111856.0 / 4481136.0)   /* the screens' refresh rate */

static ndspWaveBuf wbuf[NBUF];
static s16* data;
static s16 scratch[BUF_SAMPLES * 2];
static int ready;
static u32 next;
static double acc;

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
    ndspChnSetRate(CHANNEL, (float)CPS3_SOUND_RATE);
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
        wbuf[i].nsamples = (u32)(CPS3_SOUND_RATE / FRAME_HZ);
        ndspChnWaveBufAdd(CHANNEL, &wbuf[i]);
    }
    next = 2;
    ready = 1;
    return 1;
}

void ctr_sound_frame(void) {
    u32 n, queued = 0, i;
    ndspWaveBuf* b;
    acc += CPS3_SOUND_RATE / FRAME_HZ;
    n = (u32)acc;
    acc -= n;
    if (!ready) {
        cps3_sound_render(scratch, n);      /* no DSP: still run the voices, the game reads their state */
        return;
    }
    for (i = 0; i < NBUF; i++) {
        queued += busy(&wbuf[i]);
    }
    if (queued == 0) {
        n *= 2;                             /* ran dry after a late frame */
    } else if (queued == 1) {
        n += n / 4;
    } else if (queued >= 5) {
        n -= n / 16;
    } else if (queued == 4) {
        n -= n / 64;
    }
    b = &wbuf[next];
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

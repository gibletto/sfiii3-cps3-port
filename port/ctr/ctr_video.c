/*
 * ctr_video.c  The 3DS renderer: draws the CPS3 frame on the PICA200 GPU
 *
 * It draws what the software renderer (port/cps3/cps3_video.c) draws: the sprite list, zoomed, then the text
 * layer. How it is fast enough on an Old 3DS:
 *
 *  - Character RAM is mirrored in eight 1024x1024 8-bit textures. Only the tiles the DMA changed are updated.
 *  - The GPU has no palette textures, so fragment lighting does the lookup: the texel's value is used as the
 *    light angle, and the three colour lookup tables (RR, RG, RB) hold the palette. Changing palette is a table
 *    load, so quads are drawn grouped by palette, and the depth buffer keeps the board's drawing order.
 *  - Shadow sprites set stencil bits; the pixels under them are then drawn again from the shifted palette.
 *  - Each sprite is one 32-byte record; the geometry shader turns it into a rectangle. Writing six vertices per
 *    quad on the CPU cost more than the rest of the frame.
 *  - The frame is drawn to a texture, which is drawn zoomed to the top screen with the text layer over it.
 *
 * The sprite and tilemap walk follows cps3_video.c, derived from MAME's CPS3 driver (src/mame/capcom/cps3.cpp),
 * BSD-3-Clause, copyright-holders David Haywood, Andreas Naive, Tomasz Slanina, ElSemi.
 */

#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3_video.h"
#include "ctr_video.h"
#include "ctr_depth.h"
#include "ctr_video_shbin.h"
#include "ctr_prefetch.h"

#define DISPLAY_TRANSFER_FLAGS                                                                                   \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                            \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |              \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define RT_W        1024        /* the render target holds the board's 1024x448 line buffer */
#define RT_H        512
#define RB_W        1024
#define RB_H        448
#define BANK_DIM    1024        /* a character bank texture */
#define FG_W        256         /* the text layer's 512 8x8 tiles, 32 per row */
#define FG_H        128
#define MAX_QUADS   32768       /* sprite quads */
#define MAX_SHADOWS (MAX_QUADS / 4)
#define QPOOL       (MAX_QUADS + MAX_SHADOWS)   /* sprites from the start, then other quads; shadows from the end */
#define MAX_IDX     (QPOOL * 2)                 /* index lists */
#define Z_BASE      32767       /* far plane; a later sprite gets a larger z, so it is nearer */
#define CMDBUF_SIZE 0x100000
#define STEREO_PIXELS 1.0f      /* 3D: pixels each eye shifts per unit of depth, slider at the top */

enum { TRANS_PEN_INDEX, TRANS_PEN_INDEX_BLEND };
enum { Q_OPAQUE, Q_SHADOW64, Q_SHADOW256 };

typedef struct {
    s32 l, r, t, b;
} CLIP;

/* one quad, which is also the geometry shader's input vertex */
typedef struct {
    s16 x0, y0, x1, y1;
    float u0, v0, u1, v1;   /* texture coordinates at (x0, y0) and (x1, y1) */
    s16 z;                  /* drawing order */
    u8 bank;
    u8 kind;
    u8 bits;                /* shadows: the stencil bits it sets */
    u8 pad;
    s16 depth;              /* 3D depth (ctr_depth.h) */
} Quad;
_Static_assert(sizeof(Quad) == 32 && __builtin_offsetof(Quad, z) == 24 && __builtin_offsetof(Quad, depth) == 30,
               "the vertex layout (ctr_video_init)");

#define VREG(ofs)       cps3_be32(cps3_video_reg, ofs)
#define SPRITE_RAM32(i) cps3_be32(cps3_sprite_ram, (u32)(i) * 4)

static C3D_RenderTarget* top;
static C3D_RenderTarget* top_right;     /* the right eye, for 3D */
static C3D_RenderTarget* rt;
static s8 u_eye;
static int layered;                     /* this frame is drawn in 3D */
static float eye_shift;                 /* pixels per unit of depth (from the slider) */
static s16 walk_depth;                  /* depth for the sprites being walked */
static int walk_deep;                   /* some sprite has depth: the line buffer differs per eye */
static C3D_Tex rt_tex;
static C3D_Tex bank_tex[8];
static C3D_Tex fg_tex;
static DVLB_s* dvlb;
static shaderProgram_s prog;
static s8 u_proj;
static C3D_Mtx proj_rt, proj_top;
static C3D_LightEnv env;
static C3D_Light light;
static C3D_LightLut env_lut;
static Quad* quads;                 /* QPOOL, linear */
static u16* idx;                    /* MAX_IDX, linear */
static u32 nquads, nextra, nshadows, nidx;
#define SHADOW(i) (quads[QPOOL - 1 - (i)])
static u16 qkey[QPOOL];             /* per quad: its palette (sprites: base / 64) */
static u8 qbank[QPOOL];             /* per quad: its texture bank (sprites, and their copies under shadows) */
static u16* order;                  /* sprite quads sorted by palette */
static u32 bucket[2048 + 1];
static u32 count[2048];             /* per palette: number of quads */
static u8 bucket_banks[2048];       /* per palette: the banks its quads use, one bit each */
static u16 seq;
static u8 bucket_pens[2048];        /* per palette: the OR of its quads' pens */
static int rewalk;                  /* a pending DMA was finished during the walk: walk again */
static u32 start_count[2];          /* vertical blank counts when the wait began */
static const CLIP* walk_clip;       /* the visible part of the line buffer */

static u16 tile_swz[256];           /* tile pixel (y * 16 + x) -> offset in the bank texture */
static u16 tile_swz4[64];           /* the same for 2x2 pixel blocks, in 32-bit words */
static u8 tile_bits[0x8000];        /* per tile: the OR of its pens */
static u16 tile_vals[0x8000];       /* per tile: bit n if a pen (not 0) has low bits n; see tile_values */
static u8 tile_vals_ok[0x8000];     /* tile_vals is up to date */
static u16 shadow_vals[MAX_SHADOWS];    /* per shadow quad: the stencil values its pixels can set */
static u16 fg_prev[0x4000];         /* text layer tiles as last converted */
static u8 fg_used[512];             /* per text tile: not blank */
static u32 chan12[32];              /* a 5-bit colour channel as a LUT entry */
static u32 lut_a[256];

/* ---- LUTs ---- */

static void lut_write(GPU_LIGHTLUTID id, const u32* data) {
    GPUCMD_AddWrite(GPUREG_LIGHTING_LUT_INDEX, (u32)id << 8);
    GPUCMD_AddWrites(GPUREG_LIGHTING_LUT_DATA0, (u32*)data, 256);
}

/* how many palette entries to load for tiles whose pens OR to `pens`: 16, 64, 128 or 256 */
static u32 pen_span(u32 pens) {
    u32 n = 16;
    while (n <= pens) {
        n <<= 1;
    }
    return n;
}

/* Loads a palette into the three colour LUTs: pen c goes to entry c ^ 0x80. Only the first n pens are loaded,
   written straight into the command buffer (building arrays first was too slow on the 3DS). */
static void lut_palette(u32 base, u32 n) {
    static const u32 ids[3] = { GPU_LUT_RR, GPU_LUT_RG, GPU_LUT_RB };
    u32 e0 = n == 256 ? 0 : 0x80;           /* pens below 128 are entries 128 and up */
    u32 per = n + 3 + ((n - 1) & 1);
    u32* lut[3];
    u32 i, k;
    if (gpuCmdBuf == NULL || gpuCmdBufOffset + 3 * per > gpuCmdBufSize) {
        return;
    }
    for (i = 0; i < 3; i++) {
        u32* w = gpuCmdBuf + gpuCmdBufOffset + i * per;
        w[0] = ((u32)ids[i] << 8) | e0;
        w[1] = GPUCMD_HEADER(0, 0xF, GPUREG_LIGHTING_LUT_INDEX);
        w[3] = GPUCMD_HEADER(0, 0xF, GPUREG_LIGHTING_LUT_DATA0) | ((n - 1) << 20);
        if ((n - 1) & 1) {
            w[per - 1] = 0;
        }
        lut[i] = w;
    }
    for (k = 0; k < n; k++) {
        u32 c = (e0 + k) ^ 0x80;
        u32 col = cps3_color_ram[(c | base) & 0x1FFFF];
        u32 at = k == 0 ? 2 : k + 3;
        lut[0][at] = chan12[col & 0x1F];
        lut[1][at] = chan12[(col >> 5) & 0x1F];
        lut[2][at] = chan12[(col >> 10) & 0x1F];
    }
    gpuCmdBufOffset += 3 * per;
}

/* alpha: pens with all the mask's bits set are drawn (mask 0: every pen but 0) */
static void lut_alpha(u32 mask) {
    u32 c;
    for (c = 0; c < 256; c++) {
        u32 pass = c != 0 && (c & mask) == mask;
        lut_a[c ^ 0x80] = pass ? 0xFFF : 0;
    }
    lut_write(GPU_LUT_FR, lut_a);
}

/* ---- the geometry ---- */

/* index lists start on a 16-byte boundary */
static u32 list_start(void) {
    nidx = (nidx + 7) & ~7u;
    return nidx;
}

static void draw_list(const u16* list, u32 n) {
    if (n > 0) {
        C3D_DrawElements(GPU_GEOMETRY_PRIM, (int)n, C3D_UNSIGNED_SHORT, list);
    }
}

/* draws the list from *first, and starts a new one */
static void flush_from(u32* first) {
    draw_list(&idx[*first], nidx - *first);
    *first = list_start();
}

static void push_quad(u32 n) {
    if (nidx < MAX_IDX) {
        idx[nidx++] = (u16)n;
    }
}

/* a quad that is not a sprite: stored after the sprites */
static Quad* new_extra(void) {
    u32 n = nquads + nextra;
    if (n >= QPOOL - nshadows) {
        return NULL;
    }
    nextra++;
    return &quads[n];
}

static u32 quad_number(const Quad* q) {
    return (u32)(q - quads);
}

static void put_quad(s32 x0, s32 y0, s32 x1, s32 y1, float u0, float v0, float u1, float v1, s32 z) {
    Quad* q = new_extra();
    if (q == NULL) {
        return;
    }
    q->x0 = (s16)x0;
    q->y0 = (s16)y0;
    q->x1 = (s16)x1;
    q->y1 = (s16)y1;
    q->u0 = u0;
    q->v0 = v0;
    q->u1 = u1;
    q->v1 = v1;
    q->z = (s16)z;
    q->depth = 0;
    push_quad(quad_number(q));
}

/* an empty draw, so citro3d sends its pending state before LUTs are written directly */
static void state_flush(void) {
    u32 first = list_start();
    put_quad(0, 0, 0, 0, 0, 0, 0, 0, 0);
    flush_from(&first);
}

/* texture coordinates for a 16-texel tile scaled to n pixels, sampling the same texels as the board */
static void span16(s32 n, s32 flip, float* a, float* b) {
    s32 d = (16 << 16) / n;
    float s = d / 65536.0f;
    u32 maxf = 0;
    float delta;
    s32 k;
    for (k = 0; k < n; k++) {
        u32 f = ((u32)k * d) & 0xFFFF;
        if (f > maxf) {
            maxf = f;
        }
    }
    delta = (1.0f - maxf / 65536.0f) * 0.5f;
    if (!flip) {
        *a = delta - 0.5f * s;
        *b = *a + n * s;
    } else {
        *a = (n - 0.5f) * s + delta;
        *b = *a - n * s;
    }
}

#define SPAN_TAB 256
static float span_tab[SPAN_TAB + 1][2][2];  /* span16 precomputed: [n][flip] = { a, b } */

static void span_init(void) {
    s32 n, f;
    for (n = 1; n <= SPAN_TAB; n++) {
        for (f = 0; f < 2; f++) {
            span16(n, f, &span_tab[n][f][0], &span_tab[n][f][1]);
        }
    }
}

static void span(s32 n, s32 flip, float* a, float* b) {
    if (n <= SPAN_TAB) {
        *a = span_tab[n][flip][0];
        *b = span_tab[n][flip][1];
    } else {
        span16(n, flip, a, b);
    }
}

#define TEXU(x) ((float)(x) / BANK_DIM)
#define TEXV(y) (1.0f - (float)(y) / BANK_DIM)      /* row 0 is at v = 1 */

/* a new sprite quad; opaque ones are counted per palette for the sort */
static Quad* new_quad(s32 kind, u32 base, u32 bank, u32 pens) {
    if (kind == Q_OPAQUE) {
        if (nquads < MAX_QUADS) {
            u32 key = base >> 6;
            qkey[nquads] = (u16)key;
            qbank[nquads] = (u8)bank;
            count[key]++;
            bucket_pens[key] |= (u8)pens;
            bucket_banks[key] |= (u8)(1u << bank);
            return &quads[nquads++];
        }
    } else if (nshadows < MAX_SHADOWS) {
        Quad* q = &SHADOW(nshadows);
        nshadows++;
        return q;
    }
    return NULL;
}

/* a new walk */
static void walk_reset(void) {
    nquads = 0;
    nshadows = 0;
    seq = 1;
    walk_depth = 0;
    walk_deep = 0;
    memset(count, 0, sizeof(count));
    memset(bucket_pens, 0, sizeof(bucket_pens));
    memset(bucket_banks, 0, sizeof(bucket_banks));
}

/* bit n set if the tile has a pen (not 0) whose low four bits are n */
static u32 tile_values(u32 code) {
    if (!tile_vals_ok[code]) {
        const u8* s = &cps3_char_ram[code * 256];
        u32 k, m = 0;
        for (k = 0; k < 256; k++) {
            if (s[k] != 0) {
                m |= 1u << (s[k] & 0xF);
            }
        }
        tile_vals[code] = (u16)m;
        tile_vals_ok[code] = 1;
    }
    return tile_vals[code];
}

/* the stencil values a shadow tile's pixels set (draw_shadows): a 64-colour shadow sets the low four bits of
   the pen; a 256-colour one sets 4 for odd pens and 8 for every pen if the colour is odd */
static u32 shadow_values(u32 code, u32 color, s32 kind) {
    u32 m = tile_values(code);
    if (kind == Q_SHADOW64) {
        return m & ~1u;
    } else {
        u32 c8 = (color & 1) ? 8 : 0;
        return ((m & 0xAAAA) ? 1u << (4 | c8) : 0) | ((m & 0x5555) && c8 ? 1u << c8 : 0);
    }
}

/* a visible tile still waiting for its DMA: finish it, then walk again */
static void tile_check(u32 code, s32 x0, s32 y0, s32 x1, s32 y1) {
    if (cps3_char_tile_pending(code) && x0 <= walk_clip->r && x1 > walk_clip->l && y0 <= walk_clip->b &&
        y1 > walk_clip->t) {
        cps3_chr_dma_need(code);
        rewalk = 1;
    }
}

/* a quad showing texels [tu0, tu1) x [tv0, tv1) from a tile (reversed ranges flip). The texels may run on into
   the tiles after it in the same row of the bank texture; pens is the OR of their pens. */
static void tiles_quad(u32 code, u32 color, s32 gran, s32 trans, s32 x0, s32 y0, s32 x1, s32 y1, float tu0,
                       float tv0, float tu1, float tv1, u32 pens) {
    Quad* q;
    u32 w = code & 0xFFF;
    float bx = (float)((w & 63) * 16);
    float by = (float)((w >> 6) * 16);
    s32 kind = Q_OPAQUE;
    u32 bits = 0;
    if (trans == TRANS_PEN_INDEX_BLEND) {
        /* stencil bit n stands for colour bit 13 + n */
        if (gran == 64) {
            kind = Q_SHADOW64;
            bits = tile_bits[code] & 0xF;
        } else {
            kind = Q_SHADOW256;
            bits = ((tile_bits[code] & 1) << 2) | ((color & 1) << 3);
        }
        if (bits == 0) {
            return;
        }
    }
    q = new_quad(kind, (u32)(gran * color) & 0x1FFFF, code >> 12, pens);
    if (q == NULL) {
        return;
    }
    if (kind != Q_OPAQUE) {
        shadow_vals[nshadows - 1] = (u16)shadow_values(code, color, kind);
    }
    q->x0 = (s16)x0;
    q->y0 = (s16)y0;
    q->x1 = (s16)x1;
    q->y1 = (s16)y1;
    q->u0 = TEXU(bx + tu0);
    q->u1 = TEXU(bx + tu1);
    q->v0 = TEXV(by + tv0);
    q->v1 = TEXV(by + tv1);
    q->z = (s16)seq;
    q->bank = (u8)(code >> 12);
    q->kind = (u8)kind;
    q->bits = (u8)bits;
    q->depth = walk_depth;
    walk_deep |= walk_depth != 0;
}

static void tile_quad(u32 code, u32 color, s32 gran, s32 trans, s32 x0, s32 y0, s32 x1, s32 y1, float tu0,
                      float tv0, float tu1, float tv1) {
    if (x1 <= 0 || x0 >= RB_W || y1 <= 0 || y0 >= RB_H) {
        return;
    }
    tile_check(code, x0, y0, x1, y1);
    tiles_quad(code, color, gran, trans, x0, y0, x1, y1, tu0, tv0, tu1, tv1, tile_bits[code]);
}

/* the stencil values the shadows can leave: their own, and if two of them overlap, the ORs of those */
static u32 shadow_possible(void) {
    u32 m = 0, i, j, overlap = nshadows > 64;
    for (i = 0; i < nshadows; i++) {
        const Quad* q = &SHADOW(i);
        m |= shadow_vals[i];
        for (j = 0; j < i && !overlap; j++) {
            const Quad* o = &SHADOW(j);
            overlap = o->x0 < q->x1 && q->x0 < o->x1 && o->y0 < q->y1 && q->y0 < o->y1;
        }
    }
    if (overlap) {
        u32 grown;
        do {
            grown = m;
            for (i = 1; i < 16; i++) {
                for (j = 1; j < 16; j++) {
                    if (((m >> i) & 1) && ((m >> j) & 1)) {
                        m |= 1u << (i | j);
                    }
                }
            }
        } while (m != grown);
    }
    return m & ~1u;
}

/* quad n, or a copy of it cropped to the area (inclusive bounds), with the same texels at each pixel */
static u32 crop_quad(u32 n, const CLIP* a) {
    const Quad* q = &quads[n];
    Quad* c;
    s32 x0 = q->x0 > a->l ? q->x0 : a->l, x1 = q->x1 < a->r + 1 ? q->x1 : a->r + 1;
    s32 y0 = q->y0 > a->t ? q->y0 : a->t, y1 = q->y1 < a->b + 1 ? q->y1 : a->b + 1;
    float du, dv;
    if (x0 == q->x0 && x1 == q->x1 && y0 == q->y0 && y1 == q->y1) {
        return n;
    }
    c = new_extra();
    if (c == NULL) {
        return n;
    }
    *c = *q;
    du = (q->u1 - q->u0) / (float)(q->x1 - q->x0);
    dv = (q->v1 - q->v0) / (float)(q->y1 - q->y0);
    c->x0 = (s16)x0;
    c->x1 = (s16)x1;
    c->y0 = (s16)y0;
    c->y1 = (s16)y1;
    c->u0 = q->u0 + du * (float)(x0 - q->x0);
    c->u1 = q->u0 + du * (float)(x1 - q->x0);
    c->v0 = q->v0 + dv * (float)(y0 - q->y0);
    c->v1 = q->v0 + dv * (float)(y1 - q->y0);
    qbank[quad_number(c)] = qbank[n];
    return quad_number(c);
}

/* a 16x16 tile at (sx, sy), scaled (MAME's draw_tile16) */
static void emit_tile16(u32 code, u32 color, s32 gran, s32 flipx, s32 flipy, s32 sx, s32 sy, s32 trans,
                        s32 scalex, s32 scaley) {
    s32 sw, sh;
    float u0, u1, v0, v1;
    if (!scalex || !scaley) {
        return;
    }
    sh = (scaley * 16 + 0x8000) >> 16;
    sw = (scalex * 16 + 0x8000) >> 16;
    if (!sw || !sh) {
        return;
    }
    code %= 0x8000;
    span(sw, flipx != 0, &u0, &u1);
    span(sh, flipy != 0, &v0, &v1);
    tile_quad(code, color, gran, trans, sx, sy, sx + sw, sy + sh, u0, v0, u1, v1);
}

/* n screen lines of a tilemap that share one map row and one scroll. Opaque tiles next to each other in the bank
   texture (the next tile number, or the one before when flipped, in the same texture row, same palette and flags)
   are drawn as one quad; opaque tiles with no pixel are left out. */
static void tilemap_run(u32 mapbase, s32 xflip_mask, s32 scrollx, s32 tileline, s32 sub0, s32 dir, s32 y, s32 n,
                        const CLIP* clip) {
    s32 x, x_end = clip->r / 16 + 2;
    u32 seg_dat = 0, seg_first = 0, seg_last = 0, seg_pens = 0;
    s32 seg_n = 0, seg_x = 0;
    float tv0 = 0.0f, tv1 = 0.0f;
    for (x = clip->l / 16; x <= x_end; x++) {
        u32 dat = 0, tileno = 0;
        s32 xflip = 0, sx = x * 16 - scrollx % 16;
        int merges = 0;
        if (x < x_end) {
            dat = SPRITE_RAM32(mapbase + (tileline & 63) * 64 + (((x + scrollx / 16) & 63) ^ xflip_mask));
            tileno = ((dat & 0xFFFE0000) >> 17) % 0x8000;
            xflip = ((dat >> 12) & 1) ^ (xflip_mask & 1);
            if (!((dat >> 10) & 1)) {
                /* opaque; a tile with no pixel ends the run of tiles */
                tile_check(tileno, sx, y, sx + 16, y + n);
                merges = seg_n > 0 && tile_bits[tileno] != 0 && (dat & 0x1FFFF) == (seg_dat & 0x1FFFF) &&
                         tileno == (xflip ? seg_last - 1 : seg_last + 1) && (tileno >> 6) == (seg_last >> 6);
            }
        }
        if (merges) {
            seg_last = tileno;
            seg_pens |= tile_bits[tileno];
            seg_n++;
            continue;
        }
        if (seg_n > 0) {
            /* draw the run of tiles so far */
            s32 x0 = seg_x, x1 = seg_x + 16 * seg_n;
            s32 bpp = (seg_dat >> 9) & 1;
            if (x1 > 0 && x0 < RB_W && y + n > 0 && y < RB_H) {
                if ((seg_dat >> 12 & 1) ^ (xflip_mask & 1)) {
                    tiles_quad(seg_last, seg_dat & 0x1FF, bpp ? 64 : 256, TRANS_PEN_INDEX, x0, y, x1, y + n,
                               16.0f * seg_n, tv0, 0.0f, tv1, seg_pens);
                } else {
                    tiles_quad(seg_first, seg_dat & 0x1FF, bpp ? 64 : 256, TRANS_PEN_INDEX, x0, y, x1, y + n, 0.0f,
                               tv0, 16.0f * seg_n, tv1, seg_pens);
                }
            }
            seg_n = 0;
        }
        if (x >= x_end) {
            break;
        }
        {
            s32 yflip = (dat >> 11) & 1;
            s32 r0 = yflip ? 15 - sub0 : sub0;
            s32 step = yflip ? -dir : dir;
            float v0, v1;
            if (step >= 0) {
                v0 = (float)r0;
                v1 = (float)(r0 + n);
            } else {
                v0 = (float)(r0 + 1);
                v1 = (float)(r0 + 1 - n);
            }
            if ((dat >> 10) & 1) {
                /* shadow: one tile at a time, as each sets its own stencil bits */
                tile_quad(tileno, dat & 0x1FF, (dat >> 9) & 1 ? 64 : 256, TRANS_PEN_INDEX_BLEND, sx, y, sx + 16,
                          y + n, xflip ? 16.0f : 0.0f, v0, xflip ? 0.0f : 16.0f, v1);
            } else if (tile_bits[tileno] != 0) {
                seg_dat = dat;
                seg_first = seg_last = tileno;
                seg_pens = tile_bits[tileno];
                seg_n = 1;
                seg_x = sx;
                tv0 = v0;
                tv1 = v1;
            }
        }
    }
}

/* a tilemap sprite; runs of lines with the same row and scroll become one quad per tile */
static void emit_tilemap(s32 tilemapnum, s32 ypos2, s32 gscrolly, s32 lines, const CLIP* rclip) {
    u32 regs[4];
    s32 scrollx, scrolly, linescroll_enable, global_flip_x, global_flip_y, xflip_mask, yy, x;
    u32 linebase, mapbase;
    s32 run_y = 0, run_n = 0, run_line = 0, run_dir = 1, run_scroll = 0, run_tileline = 0, run_sub = 0;
    s32 prev_line = 0;
    for (x = 0; x < 4; x++) {
        regs[x] = VREG(0x20 + tilemapnum * 16 + x * 4);
    }
    if (!(regs[1] & 0x8000)) {
        return;
    }
    scrollx = (regs[0] & 0xFFFF0000) >> 16;
    scrolly = (regs[0] & 0xFFFF) + 4;
    linescroll_enable = (regs[1] >> 14) & 1;
    global_flip_x = (regs[1] >> 11) & 1;
    global_flip_y = (regs[1] >> 10) & 1;
    linebase = ((regs[2] & 0x7F000000) >> 24) << 10;
    mapbase = ((regs[2] & 0x007F0000) >> 16) << 10;
    xflip_mask = global_flip_x ? 0x3F : 0;
    for (yy = 0; yy < lines; yy++) {
        s32 drawline = (~(ypos2 + gscrolly - yy) - 18) & 0x3FF;
        s32 line, sx, tileline;
        if (drawline < rclip->t || drawline > rclip->b) {
            continue;
        }
        line = (drawline + scrolly) & 0x3FF;
        if (global_flip_y) {
            line ^= 0x3FF;
        }
        sx = scrollx;
        if (linescroll_enable) {
            sx += (SPRITE_RAM32(linebase + ((line + 16) & 0x3FF)) >> 16) & 0x3FF;
        }
        tileline = line / 16 + 1;
        if (run_n > 0 && drawline == run_y + run_n && tileline == run_tileline && sx == run_scroll &&
            (run_n == 1 ? (line - prev_line == 1 || line - prev_line == -1) : line - prev_line == run_dir)) {
            if (run_n == 1) {
                run_dir = line - prev_line;
            }
            run_n++;
            prev_line = line;
            continue;
        }
        if (run_n > 0) {
            tilemap_run(mapbase, xflip_mask, run_scroll, run_tileline, run_sub, run_dir, run_y, run_n, rclip);
        }
        run_y = drawline;
        run_n = 1;
        run_line = line;
        run_dir = 1;
        run_scroll = sx;
        run_tileline = tileline;
        run_sub = line % 16;
        prev_line = line;
    }
    if (run_n > 0) {
        tilemap_run(mapbase, xflip_mask, run_scroll, run_tileline, run_sub, run_dir, run_y, run_n, rclip);
    }
    (void)run_line;
}

/* the sprite list (MAME's draw_sprites) */
static void walk_sprites(const CLIP* rclip) {
    static const s32 tilestable[4] = { 8, 1, 2, 4 };
    const u32* sl = cps3_spritelist;
    s32 i, j;
    for (i = 0; i < 0x2000 / 4; i += 4) {
        u32 gscroll, length, start, whichbpp, whichpal, global_xflip, global_yflip, global_alpha, global_bpp, global_pal;
        s32 xpos, ypos, gscrollx, gscrolly;
        if (sl[i] & 0x80000000) {
            break;
        }
        gscroll = (sl[i] & 0x70000000) >> 28;
        length = (sl[i] & 0x01FF0000) >> 16;
        start = (sl[i] & 0x00007FF0) >> 4;
        xpos = (sl[i + 1] & 0x03FF0000) >> 16;
        ypos = sl[i + 1] & 0x3FF;
        whichbpp = (sl[i + 2] >> 30) & 1;
        whichpal = (sl[i + 2] >> 29) & 1;
        global_xflip = (sl[i + 2] >> 28) & 1;
        global_yflip = (sl[i + 2] >> 27) & 1;
        global_alpha = (sl[i + 2] >> 26) & 1;
        global_bpp = (sl[i + 2] >> 25) & 1;
        global_pal = (sl[i + 2] & 0x01FF0000) >> 16;
        gscrollx = (cps3_gscroll_buff[gscroll] & 0x03FF0000) >> 16;
        gscrolly = cps3_gscroll_buff[gscroll] & 0x3FF;
        start = (start * 0x100) >> 2;

        for (j = 0; j < (s32)length * 4; j += 4) {
            u32 value1, value2, value3, tileno, flipx, flipy, alpha, bpp, pal, ysizedraw2, xsizedraw2;
            s32 xpos2, ypos2, ysize2, xsize2;
            if (start + j + 2 >= CPS3_SPRITE_RAM_SIZE / 4) {
                break;
            }
            value1 = sl[start + j];
            value2 = sl[start + j + 1];
            value3 = sl[start + j + 2];
            tileno = (value1 & 0xFFFE0000) >> 17;
            flipx = (value1 >> 12) & 1;
            flipy = (value1 >> 11) & 1;
            alpha = (value1 >> 10) & 1;
            bpp = (value1 >> 9) & 1;
            pal = value1 & 0x1FF;
            xpos2 = (value2 & 0x03FF0000) >> 16;
            ypos2 = value2 & 0x3FF;
            ysizedraw2 = ((value3 & 0x7F000000) >> 24) + 1;
            xsizedraw2 = ((value3 & 0x007F0000) >> 16) + 1;
            ysize2 = (value3 >> 2) & 3;
            xsize2 = value3 & 3;
            if (ysize2 == 0) {
                continue;
            }
            if (seq < Z_BASE - 1) {
                seq++;
            }
            if (xsize2 == 0) {
                if (layered) {
                    walk_depth = ctr_depth_sprite(0, 1);
                }
                emit_tilemap((value3 >> 4) & 3, ypos2, gscrolly, (s32)ysizedraw2, rclip);
            } else {
                u32 xinc, yinc, xscale, yscale, actualpal;
                s32 granularity, trans, count, xx, yy;
                ysize2 = tilestable[ysize2];
                xsize2 = tilestable[xsize2];
                xinc = (xsizedraw2 << 16) / xsize2;
                yinc = (ysizedraw2 << 16) / ysize2;
                xscale = xinc / 16;
                yscale = yinc / 16;
                if (xscale & 0xFFFF) {
                    xscale += (1 << 16) / 16;
                }
                if (yscale & 0xFFFF) {
                    yscale += (1 << 16) / 16;
                }
                xsize2 -= 1;
                ysize2 -= 1;
                flipx ^= global_xflip;
                flipy ^= global_yflip;
                if (!flipx) {
                    xpos2 += xsizedraw2 / 2;
                } else {
                    xpos2 -= xsizedraw2 / 2;
                }
                ypos2 += ysizedraw2 / 2;
                if (!flipx) {
                    xpos2 -= ((xsize2 + 1) * xinc) >> 16;
                } else {
                    xpos2 += (xsize2 * xinc) >> 16;
                }
                if (flipy) {
                    ypos2 -= (ysize2 * yinc) >> 16;
                }
                actualpal = whichpal ? global_pal : pal;
                if (layered) {
                    walk_depth = ctr_depth_sprite(actualpal, 0);
                }
                granularity = (whichbpp ? global_bpp : bpp) ? 64 : 256;
                trans = (global_alpha || alpha) ? TRANS_PEN_INDEX_BLEND : TRANS_PEN_INDEX;
                count = 0;
                for (xx = 0; xx < xsize2 + 1; xx++) {
                    s32 current_xpos;
                    if (!flipx) {
                        current_xpos = xpos + xpos2 + ((xx * xinc) >> 16);
                    } else {
                        current_xpos = xpos + xpos2 - ((xx * xinc) >> 16);
                    }
                    current_xpos += gscrollx;
                    current_xpos += 1;
                    current_xpos &= 0x3FF;
                    if (current_xpos & 0x200) {
                        current_xpos -= 0x400;
                    }
                    for (yy = 0; yy < ysize2 + 1; yy++) {
                        s32 current_ypos;
                        if (flipy) {
                            current_ypos = ypos + ypos2 + ((yy * yinc) >> 16);
                        } else {
                            current_ypos = ypos + ypos2 - ((yy * yinc) >> 16);
                        }
                        current_ypos += gscrolly;
                        current_ypos = 0x3FF - current_ypos;
                        current_ypos -= 17;
                        current_ypos &= 0x3FF;
                        if (current_ypos & 0x200) {
                            current_ypos -= 0x400;
                        }
                        emit_tile16(tileno + count, actualpal, granularity, flipx, flipy, current_xpos, current_ypos,
                                    trans, xscale, yscale);
                        count++;
                    }
                }
            }
        }
    }
}

/* ---- textures ---- */

static u32 morton8(u32 x, u32 y) {
    u32 m = 0;
    u32 i;
    for (i = 0; i < 3; i++) {
        m |= ((x >> i) & 1) << (2 * i);
        m |= ((y >> i) & 1) << (2 * i + 1);
    }
    return m;
}

/* copies the tiles the character DMA changed into the bank textures */
static void sync_char(void) {
    u32 lo[8], hi[8];
    u32 i, b;
    for (b = 0; b < 8; b++) {
        lo[b] = 0xFFFFFFFF;
        hi[b] = 0;
    }
    for (i = 0; i < sizeof(cps3_char_dirty); i++) {
        u32 bits;
        if ((i & 3) == 0 && i + 4 <= sizeof(cps3_char_dirty)) {
            u32 word;
            memcpy(&word, &cps3_char_dirty[i], 4);
            if (word == 0) {
                i += 3;                 /* mostly zero: skip a word at a time */
                continue;
            }
        }
        bits = cps3_char_dirty[i];
        if (bits == 0) {
            continue;
        }
        cps3_char_dirty[i] = 0;
        for (b = 0; b < 8; b++) {
            if (bits & (1u << b)) {
                u32 t = i * 8 + b;
                u32 w = t & 0xFFF;
                u32 bank = t >> 12;
                u32 base = ((w >> 6) * 256 + (w & 63) * 2) * 64;
                const u8* src = &cps3_char_ram[t * 256];
                u32* dst = (u32*)((u8*)bank_tex[bank].data + base);
                u32 orbits = 0;
                u32 y, x;
                /* two rows at a time, as 2x2 blocks of one word each */
                for (y = 0; y < 8; y++) {
                    const u8* r0 = src + y * 32;
                    for (x = 0; x < 4; x++) {
                        u32 a, b;
                        memcpy(&a, r0 + x * 4, 4);
                        memcpy(&b, r0 + 16 + x * 4, 4);
                        dst[tile_swz4[y * 8 + x * 2]] = (a & 0xFFFF) | (b << 16);
                        dst[tile_swz4[y * 8 + x * 2 + 1]] = (a >> 16) | (b & 0xFFFF0000);
                        orbits |= a | b;
                    }
                }
                orbits |= orbits >> 16;
                orbits |= orbits >> 8;
                tile_bits[t] = (u8)orbits;
                tile_vals_ok[t] = 0;
                if (base < lo[bank]) {
                    lo[bank] = base;
                }
                if (base + 8192 + 128 > hi[bank]) {
                    hi[bank] = base + 8192 + 128;
                }
            }
        }
    }
    for (b = 0; b < 8; b++) {
        if (hi[b] > lo[b]) {
            GSPGPU_FlushDataCache((u8*)bank_tex[b].data + lo[b], hi[b] - lo[b]);
        }
    }
}

static u8 ss_byte(u32 n) {
    return (u8)cps3_ss_ram[n & 0x7FFF];
}

/* do two 64-byte tiles differ? (both word aligned) */
static int tile64_differs(const u32* a, const u32* b) {
    u32 d = 0, i;
    __builtin_prefetch(a + 16);
    __builtin_prefetch(b + 16);
    for (i = 0; i < 16; i += 4) {
        d |= (a[i] ^ b[i]) | (a[i + 1] ^ b[i + 1]) | (a[i + 2] ^ b[i + 2]) | (a[i + 3] ^ b[i + 3]);
    }
    return d != 0;
}

/* converts the text layer's 4-bit tiles into an 8-bit texture */
static void sync_fg(void) {
    u32 t;
    int changed = 0;
    const int aligned = (((uintptr_t)&cps3_ss_ram[0x4000] | (uintptr_t)fg_prev) & 3) == 0;
    for (t = 0; t < 512; t++) {
        const u16* src = &cps3_ss_ram[0x4000 + t * 32];
        if (aligned ? tile64_differs((const u32*)src, (const u32*)&fg_prev[t * 32])
                    : memcmp(src, &fg_prev[t * 32], 64) != 0) {
            u8* dst = (u8*)fg_tex.data + t * 64;
            u32 x, y, any = 0;
            memcpy(&fg_prev[t * 32], src, 64);
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x++) {
                    u8 b = ss_byte(0x4000 + t * 32 + y * 4 + x / 2);
                    u8 c = (x & 1) ? (b >> 4) : (b & 0xF);
                    dst[morton8(x, y)] = c;
                    any |= c;
                }
            }
            fg_used[t] = any != 0;
            changed = 1;
        }
    }
    if (changed) {
        GSPGPU_FlushDataCache(fg_tex.data, FG_W * FG_H);
    }
}

/* ---- passes ---- */

static void texenv_palette(void) {
    C3D_TexEnv* te = C3D_GetTexEnv(0);
    C3D_TexEnvInit(te);
    C3D_TexEnvSrc(te, C3D_Both, GPU_FRAGMENT_SECONDARY_COLOR, 0, 0);
    C3D_TexEnvFunc(te, C3D_Both, GPU_REPLACE);
}

static void texenv_texture(void) {
    C3D_TexEnv* te = C3D_GetTexEnv(0);
    C3D_TexEnvInit(te);
    C3D_TexEnvSrc(te, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(te, C3D_Both, GPU_REPLACE);
}

static void texenv_constant(u32 abgr) {
    C3D_TexEnv* te = C3D_GetTexEnv(0);
    C3D_TexEnvInit(te);
    C3D_TexEnvSrc(te, C3D_Both, GPU_CONSTANT, 0, 0);
    C3D_TexEnvColor(te, abgr);
    C3D_TexEnvFunc(te, C3D_Both, GPU_REPLACE);
}

static void bind_bank(s32 b, s32* bank) {
    if (b != *bank) {
        *bank = b;
        C3D_TexBind(0, &bank_tex[b]);
    }
}

/* draws n quads of one palette, switching bank textures as needed */
static void draw_range(const u16* list, u32 n, u32 banks, s32* bank) {
    u32 k, from;
    if (n == 0) {
        return;
    }
    if ((banks & (banks - 1)) == 0) {
        bind_bank((s32)__builtin_ctz(banks), bank);
        draw_list(list, n);
        return;
    }
    for (from = 0, k = 0; k < n; k++) {
        if (qbank[list[k]] != *bank) {
            draw_list(&list[from], k - from);
            from = k;
            bind_bank(qbank[list[k]], bank);
        }
    }
    draw_list(&list[from], n - from);
}

static u32 colour_abgr(u16 col) {
    u32 r = (col & 0x1F) << 3, g = ((col >> 5) & 0x1F) << 3, b = ((col >> 10) & 0x1F) << 3;
    return 0xFF000000 | (b << 16) | (g << 8) | r;
}

/* shadows: set their stencil bits, then redraw the pixels under them with the shifted palettes */
static void draw_shadows(void) {
    static u32 fbound[2048 + 1];    /* per palette: start of its quads under the shadows */
    CLIP area = { 0x7FFF, -0x7FFF, 0x7FFF, -0x7FFF };
    u32 setbits = 0, possible, v, i, b, k;
    s32 bank = -1;
    u32 first;

    /* set stencil bits where a shadow is over the pixel; no colour or depth writes */
    C3D_DepthTest(true, GPU_LESS, 0);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    for (b = 0; b < 4; b++) {
        int any = 0;
        for (i = 0; i < nshadows && !any; i++) {
            any = (SHADOW(i).bits >> b) & 1;
        }
        if (!any) {
            continue;
        }
        C3D_StencilTest(true, GPU_ALWAYS, 0xFF, 0xFF, 1 << b);
        state_flush();
        for (k = 0; k < 2; k++) {
            /* k = 0: 64-colour shadows; 1: 256-colour shadows */
            u32 mask = k == 0 ? (1u << b) : (b == 2 ? 1u : 0u);
            if (k == 1 && b < 2) {
                continue;
            }
            lut_alpha(mask);
            first = list_start();
            for (i = 0; i < nshadows; i++) {
                const Quad* q = &SHADOW(i);
                if (q->kind != (k == 0 ? Q_SHADOW64 : Q_SHADOW256) || !((q->bits >> b) & 1)) {
                    continue;
                }
                if (q->bank != bank) {
                    flush_from(&first);
                    bind_bank(q->bank, &bank);
                }
                push_quad(quad_number(q));
                if (q->x0 < area.l) {
                    area.l = q->x0;
                }
                if (q->x1 - 1 > area.r) {
                    area.r = q->x1 - 1;
                }
                if (q->y0 < area.t) {
                    area.t = q->y0;
                }
                if (q->y1 - 1 > area.b) {
                    area.b = q->y1 - 1;
                }
                setbits |= 1u << b;
            }
            flush_from(&first);
        }
    }
    if (setbits == 0) {
        return;
    }
    possible = shadow_possible();

    /* the sprite quads under the shadows, by palette, cropped to the shadows' box */
    list_start();
    for (i = 0; i < 2048; i++) {
        u32 j;
        fbound[i] = nidx;
        for (j = bucket[i]; j < bucket[i + 1]; j++) {
            const Quad* q = &quads[order[j]];
            if (q->x1 <= area.l || q->x0 > area.r || q->y1 <= area.t || q->y0 > area.b) {
                continue;
            }
            push_quad(crop_quad(order[j], &area));
        }
        fbound[i + 1] = nidx;
    }

    /* for each stencil value, redraw those pixels from colour RAM (pen | base | v << 13) */
    lut_alpha(0);
    for (v = 1; v < 16; v++) {
        if ((v & setbits) != v || !((possible >> v) & 1)) {
            continue;
        }
        C3D_StencilTest(true, GPU_EQUAL, (int)v, 0xF, 0);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);

        /* the background colour, behind every sprite */
        C3D_DepthTest(true, GPU_LESS, GPU_WRITE_COLOR);
        texenv_constant(colour_abgr(cps3_color_ram[(v << 13) & 0x1FFFF]));
        first = list_start();
        put_quad(area.l, area.t, area.r + 1, area.b + 1, 0, 0, 0, 0, 1);
        flush_from(&first);

        texenv_palette();
        C3D_DepthTest(true, GPU_EQUAL, GPU_WRITE_COLOR);
        state_flush();
        for (i = 0; i < 2048; i++) {
            if (fbound[i] == fbound[i + 1]) {
                continue;
            }
            lut_palette((i << 6) | (v << 13), pen_span(bucket_pens[i]));
            draw_range(&idx[fbound[i]], fbound[i + 1] - fbound[i], bucket_banks[i], &bank);
        }
    }
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
}

/* sorts the opaque sprite quads by palette (a counting sort) into the index list */
static void sort_opaque(void) {
    u32 i;
    bucket[0] = 0;
    for (i = 0; i < 2048; i++) {
        bucket[i + 1] = bucket[i] + count[i];
        count[i] = bucket[i];
    }
    order = &idx[list_start()];
    if (nidx + nquads > MAX_IDX) {
        nquads = 0;
        memset(bucket, 0, sizeof(bucket));
        return;
    }
    for (i = 0; i < nquads; i++) {
        order[count[qkey[i]]++] = (u16)i;
    }
    nidx += nquads;
}

static void draw_opaque(void) {
    s32 bank = -1;
    u32 i;
    C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
    for (i = 0; i < 2048; i++) {
        if (bucket[i] == bucket[i + 1]) {
            continue;
        }
        lut_palette(i << 6, pen_span(bucket_pens[i]));
        draw_range(&order[bucket[i]], bucket[i + 1] - bucket[i], bucket_banks[i], &bank);
    }
}

static u16 fq_count[33];           /* text quads: where each palette's run ends in fg_list */
static u16* fg_list;

/* builds the text layer's quads at (ox, oy) on the top screen, sorted by palette */
static void build_fg(s32 ox, s32 oy, s32 w, s32 h) {
    u16 vscroll = (u16)(((u8)cps3_ss_reg[0x11] << 8) | (u8)cps3_ss_reg[0x10]);
    s32 scrolly = (-vscroll) & 0x100;
    s32 line, p;
    u32 n_fg = 0, i, fbase = nquads + nextra;
    u16* list;
    memset(fq_count, 0, sizeof(fq_count));
    for (line = 0; line < h;) {
        s32 y = line / 8;
        s32 rowscroll = ss_byte(((line + scrolly - 1) & 0x1FF) * 2 + 0x2000);
        s32 offset = ((line + scrolly) / 8 * 128) & 0x1FFF;
        s32 n = 1, x, copy;
        while (line + n < h && (line + n) / 8 == y &&
               ss_byte(((line + n + scrolly - 1) & 0x1FF) * 2 + 0x2000) == rowscroll) {
            n++;
        }
        for (x = 0; x < 64; x++) {
            u16 data = ss_byte(offset + x * 2) | (ss_byte(offset + x * 2 + 1) << 8);
            u32 tile = data & 0x1FF;
            s32 pal = (data & 0x3E00) >> 9;
            s32 flipy = (data >> 14) & 1;
            s32 flipx = (data >> 15) & 1;
            s32 r0 = line - y * 8;
            float tv0, tv1, tu0, tu1;
            if (!fg_used[tile]) {
                continue;
            }
            if (!flipy) {
                tv0 = (float)r0;
                tv1 = (float)(r0 + n);
            } else {
                tv0 = (float)(8 - r0);
                tv1 = (float)(8 - r0 - n);
            }
            for (copy = 0; copy < 2; copy++) {
                s32 dx = copy * 512 + x * 8 - rowscroll;
                s32 x0 = dx < 0 ? 0 : dx;
                s32 x1 = dx + 8 > w ? w : dx + 8;
                float bx = (float)((tile & 31) * 8), by = (float)((tile >> 5) * 8);
                Quad* q;
                if (x1 <= x0) {
                    continue;
                }
                q = new_extra();
                if (q == NULL) {
                    continue;
                }
                n_fg++;
                tu0 = (float)(flipx ? 8 - (x0 - dx) : x0 - dx);
                tu1 = (float)(flipx ? 8 - (x1 - dx) : x1 - dx);
                q->x0 = (s16)(ox + x0);
                q->y0 = (s16)(oy + line);
                q->x1 = (s16)(ox + x1);
                q->y1 = (s16)(oy + line + n);
                q->u0 = (bx + tu0) / FG_W;
                q->v0 = 1.0f - (by + tv0) / FG_H;
                q->u1 = (bx + tu1) / FG_W;
                q->v1 = 1.0f - (by + tv1) / FG_H;
                q->z = 0;
                q->depth = layered ? ctr_depth_text(y, (u32)pal) : 0;
                qkey[quad_number(q)] = (u16)pal;
                fq_count[pal + 1]++;
            }
        }
        line += n;
    }
    for (p = 0; p < 32; p++) {
        fq_count[p + 1] += fq_count[p];         /* now the start of each palette */
    }
    list = &idx[list_start()];
    fg_list = list;
    if (nidx + n_fg > MAX_IDX) {
        memset(fq_count, 0, sizeof(fq_count));
        return;
    }
    nidx += n_fg;
    for (i = fbase; i < fbase + n_fg; i++) {
        list[fq_count[qkey[i]]++] = (u16)i;
    }
    /* now fq_count[p] is the end of palette p */
}

static void draw_fg(void) {
    u32 pal_base = (u8)cps3_ss_reg[0x12];
    s32 p;
    for (p = 0; p < 32; p++) {
        u32 from = p ? fq_count[p - 1] : 0, to = fq_count[p];
        if (from == to) {
            continue;
        }
        lut_palette(((u32)p + (pal_base << 5)) * 16, 16);   /* 4-bit pens */
        draw_list(&fg_list[from], to - from);
    }
}

/* ---- the frame ---- */

/* flushes the quads and index lists from the CPU cache for the GPU */
static void flush_quads(void) {
    GSPGPU_FlushDataCache(quads, (nquads + nextra) * sizeof(Quad));
    if (nshadows > 0) {
        GSPGPU_FlushDataCache(&SHADOW(nshadows - 1), nshadows * sizeof(Quad));
    }
    GSPGPU_FlushDataCache(idx, nidx * sizeof(u16));
}

void ctr_video_render(void) {
    u32 zx = VREG(0x6C) & 0xFF;
    u32 zy = VREG(0x7C) & 0xFF;
    u32 fszx, fszy;
    s32 w, h, ox, oy;
    CLIP rclip;
    u32 first;
    s32 eye;
    float su, sv, u0, v0;

    /* Wait for the vertical blank on both screens (as C3D_FrameSync does), running queued character DMA in
       small pieces meanwhile. */
    start_count[0] = C3D_FrameCounter(0);
    start_count[1] = C3D_FrameCounter(1);
    while (C3D_FrameCounter(0) == start_count[0] || C3D_FrameCounter(1) == start_count[1]) {
        if (cps3_chr_jobs > 0) {
            if (!cps3_chr_dma_work_loaded(CHR_IDLE_PIECE)) {
                /* its page is still being read (by the read-ahead thread): never wait for the SD card here */
                svcSleepThread(500000);
                ctr_prefetch_poll();
            }
        } else {
            gspWaitForAnyEvent();
        }
    }
    C3D_FrameBegin(0);
    walk_reset();
    nextra = 0;
    nidx = 0;

    sync_char();
    sync_fg();

    /* 3D, when the slider is up and the screen has depth (ctr_depth.c) */
    {
        float slider = top_right != NULL ? osGet3DSliderState() : 0.0f;
        layered = slider > 0.0f && ctr_depth_frame();
        eye_shift = slider * STEREO_PIXELS;
    }

    cps3_video_size(&w, &h);
    if (zx > 0x80) {
        zx = 0x80;
    }
    if (zy > 0x80) {
        zy = 0x80;
    }
    fszx = (zx << 16) / 0x40;
    fszy = (zy << 16) / 0x40;
    rclip.l = 0;
    rclip.t = 0;
    if (zx == 0x40 && zy == 0x40) {
        rclip.r = w - 1;
        rclip.b = h - 1;
    } else {
        rclip.r = (s32)(((u32)w * fszx + 0x8000) >> 16) - 1;
        rclip.b = (s32)(((u32)h * fszy + 0x8000) >> 16) - 1;
    }
    if (rclip.r >= RB_W) {
        rclip.r = RB_W - 1;
    }
    if (rclip.b >= RB_H) {
        rclip.b = RB_H - 1;
    }
    walk_clip = &rclip;
    walk_sprites(&rclip);
    while (rewalk) {
        rewalk = 0;
        sync_char();
        walk_reset();
        walk_sprites(&rclip);
    }

    /* index lists: sprites by palette, then the text layer */
    sort_opaque();
    ox = (400 - w) / 2;
    oy = (240 - h) / 2;
    if (ox < 0) {
        ox = 0;
    }
    if (oy < 0) {
        oy = 0;
    }
    build_fg(ox, oy, w, h);
    su = fszx / 65536.0f;
    sv = fszy / 65536.0f;
    u0 = (zx == 0x40) ? 0.0f : -0.5f * su + 1.0f / 256;
    v0 = (zy == 0x40) ? 0.0f : -0.5f * sv + 1.0f / 256;

    /* Each eye: the line buffer, then the top screen. In 2D only the left eye is drawn and shown to both. In 3D
       each eye shifts layers the opposite way; the line buffer is redrawn only if sprites have depth. */
    for (eye = 0; eye < (layered ? 2 : 1); eye++) {
        C3D_RenderTarget* screen = eye ? top_right : top;
        float shift = layered ? (eye ? -eye_shift : eye_shift) : 0.0f;
        if (eye == 0 || walk_deep) {
            /* the line buffer: clear to colour 0, far depth, stencil 0 */
            u16 c0 = cps3_color_ram[0];
            u32 rgba = ((u32)(c0 & 0x1F) << 27) | ((u32)((c0 >> 5) & 0x1F) << 19) |
                       ((u32)((c0 >> 10) & 0x1F) << 11) | 0xFF;
            if (eye) {
                flush_quads();
                C3D_FrameSplit(0);      /* send the left eye's commands before the clear */
            }
            C3D_RenderTargetClear(rt, C3D_CLEAR_ALL, rgba, 0x00FFFFFF);
            C3D_FrameDrawOn(rt);
            C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, u_proj, &proj_rt);
            C3D_FVUnifSet(GPU_GEOMETRY_SHADER, u_eye, shift, 0.0f, 0.0f, 0.0f);
            C3D_LightEnvBind(&env);
            texenv_palette();
            C3D_AlphaTest(true, GPU_GREATER, 0);
            C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
            state_flush();
            lut_alpha(0);
            draw_opaque();
            if (nshadows > 0) {
                draw_shadows();
            }
        }

        /* the top screen: the line buffer zoomed, then the text layer */
        C3D_RenderTargetClear(screen, C3D_CLEAR_COLOR, 0x000000FF, 0);
        C3D_FrameDrawOn(screen);
        C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, u_proj, &proj_top);
        C3D_FVUnifSet(GPU_GEOMETRY_SHADER, u_eye, shift, 0.0f, 0.0f, 0.0f);
        C3D_LightEnvBind(NULL);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        texenv_texture();
        C3D_TexBind(0, &rt_tex);
        first = list_start();
        put_quad(ox, oy, ox + w, oy + h, u0 / RT_W, 1.0f - v0 / RT_H, (u0 + w * su) / RT_W,
                 1.0f - (v0 + h * sv) / RT_H, 0);
        flush_from(&first);

        C3D_LightEnvBind(&env);
        texenv_palette();
        C3D_AlphaTest(true, GPU_GREATER, 0);
        C3D_TexBind(0, &fg_tex);
        state_flush();
        lut_alpha(0);
        draw_fg();
    }

    flush_quads();
    C3D_FrameEnd(0);
}

static void light_setup(void) {
    static const C3D_Material mtl = {
        { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 1, 1, 1 }, { 0, 0, 0 },
    };
    static float zero[512];
    C3D_FVec pos = FVec4_New(1.0f, 0.0f, 0.0f, 0.0f);   /* along +x, so L.N is the normal's x */
    LightLut_FromArray(&env_lut, zero);
    C3D_LightEnvInit(&env);
    C3D_LightEnvMaterial(&env, &mtl);
    C3D_LightEnvAmbient(&env, 0, 0, 0);
    C3D_LightEnvLut(&env, GPU_LUT_D0, GPU_LUTINPUT_NH, false, NULL);
    C3D_LightEnvLut(&env, GPU_LUT_D1, GPU_LUTINPUT_NH, false, NULL);
    C3D_LightEnvLut(&env, GPU_LUT_FR, GPU_LUTINPUT_LN, true, &env_lut);
    C3D_LightEnvLut(&env, GPU_LUT_RR, GPU_LUTINPUT_LN, true, &env_lut);
    C3D_LightEnvLut(&env, GPU_LUT_RG, GPU_LUTINPUT_LN, true, &env_lut);
    C3D_LightEnvLut(&env, GPU_LUT_RB, GPU_LUTINPUT_LN, true, &env_lut);
    C3D_LightEnvFresnel(&env, GPU_SEC_ALPHA_FRESNEL);
    C3D_LightEnvBumpMode(&env, GPU_BUMP_AS_BUMP);
    C3D_LightEnvBumpSel(&env, 0);
    C3D_LightEnvBumpNormalZ(&env, false);
    C3D_LightEnvClampHighlights(&env, false);
    C3D_LightInit(&light, &env);
    C3D_LightColor(&light, 0, 0, 0);
    C3D_LightSpecular0(&light, 0, 0, 0);
    C3D_LightSpecular1(&light, 1, 1, 1);
    C3D_LightPosition(&light, &pos);
    C3D_LightShadowEnable(&light, false);
    C3D_LightSpotEnable(&light, false);
    C3D_LightDistAttnEnable(&light, false);
    C3D_LightGeoFactor(&light, 0, false);
    C3D_LightGeoFactor(&light, 1, false);
    /* layer configuration 5, which has the Fresnel LUT used for alpha (citro3d sets 4) */
    env.conf.config[0] = (env.conf.config[0] & ~(0xFu << 4)) | (GPU_LIGHT_ENV_LAYER_CONFIG(5) << 4);
    env.flags |= C3DF_LightEnv_Dirty;
}

int ctr_video_init(void) {
    C3D_AttrInfo* attr;
    C3D_BufInfo* buf;
    u32 i, x, y;

    if (!C3D_Init(CMDBUF_SIZE)) {
        return 0;
    }
    C3D_CullFace(GPU_CULL_NONE);
    top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    if (!C3D_TexInitVRAM(&rt_tex, RT_W, RT_H, GPU_RGBA8)) {
        return 0;
    }
    C3D_TexSetFilter(&rt_tex, GPU_NEAREST, GPU_NEAREST);
    rt = C3D_RenderTargetCreateFromTex(&rt_tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
    if (top == NULL || rt == NULL) {
        return 0;
    }
    /* the right eye, if VRAM allows; a frame that draws only the left eye is shown to both */
    top_right = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
    if (top_right != NULL) {
        C3D_RenderTargetSetOutput(top_right, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
        gfxSet3D(true);
    }
    for (i = 0; i < 8; i++) {
        if (!C3D_TexInit(&bank_tex[i], BANK_DIM, BANK_DIM, GPU_L8)) {
            return 0;
        }
        C3D_TexSetFilter(&bank_tex[i], GPU_NEAREST, GPU_NEAREST);
    }
    if (!C3D_TexInit(&fg_tex, FG_W, FG_H, GPU_L8)) {
        return 0;
    }
    C3D_TexSetFilter(&fg_tex, GPU_NEAREST, GPU_NEAREST);
    memset(fg_tex.data, 0, FG_W * FG_H);
    memset(fg_prev, 0xFF, sizeof(fg_prev));     /* convert everything on the first frame */
    memset(cps3_char_dirty, 0xFF, sizeof(cps3_char_dirty));

    quads = linearAlloc(sizeof(Quad) * QPOOL);
    idx = linearAlloc(sizeof(u16) * MAX_IDX);
    if (quads == NULL || idx == NULL) {
        return 0;
    }

    /* one vertex per quad; the geometry shader makes the rectangle */
    dvlb = DVLB_ParseFile((u32*)ctr_video_shbin, ctr_video_shbin_size);
    shaderProgramInit(&prog);
    shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
    shaderProgramSetGsh(&prog, &dvlb->DVLE[1], 0);
    C3D_BindProgram(&prog);
    u_proj = shaderInstanceGetUniformLocation(prog.geometryShader, "projection");
    u_eye = shaderInstanceGetUniformLocation(prog.geometryShader, "eye");
    Mtx_Ortho(&proj_rt, 0.0f, RT_W, RT_H, 0.0f, 0.0f, Z_BASE + 1, true);
    Mtx_OrthoTilt(&proj_top, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, Z_BASE + 1, true);

    attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);      /* x0, y0, x1, y1 */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 4);      /* u0, v0, u1, v1 */
    AttrInfo_AddLoader(attr, 2, GPU_SHORT, 4);      /* z, bank and kind, bits, depth */
    buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, quads, sizeof(Quad), 3, 0x210);

    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            tile_swz[y * 16 + x] = (u16)(((y >> 3) * (BANK_DIM / 8) + (x >> 3)) * 64 + morton8(x & 7, y & 7));
        }
    }
    for (i = 0; i < 64; i++) {
        tile_swz4[i] = (u16)(tile_swz[(i >> 3) * 32 + (i & 7) * 2] / 4);
    }
    for (i = 0; i < 32; i++) {
        u32 v = (i << 3) * 4096 / 255;
        chan12[i] = v > 0xFFF ? 0xFFF : v;
    }
    light_setup();
    span_init();
    return 1;
}

void ctr_video_exit(void) {
    u32 i;
    for (i = 0; i < 8; i++) {
        C3D_TexDelete(&bank_tex[i]);
    }
    C3D_TexDelete(&fg_tex);
    C3D_RenderTargetDelete(rt);
    C3D_RenderTargetDelete(top);
    if (top_right != NULL) {
        C3D_RenderTargetDelete(top_right);
    }
    C3D_TexDelete(&rt_tex);
    linearFree(quads);
    linearFree(idx);
    shaderProgramFree(&prog);
    DVLB_Free(dvlb);
    C3D_Fini();
}

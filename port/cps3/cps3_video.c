/*
 * cps3_video.c  Reference software renderer for the CPS3 video hardware
 *
 * Sprites and tilemaps are drawn as colour indexes, so alpha effects can OR bits into them.
 * The buffer is then zoomed and converted through colour RAM, and the text layer is drawn on top.
 *
 * Derived from MAME's cps3 driver (src/mame/capcom/cps3.cpp), BSD-3-Clause,
 * copyright-holders David Haywood, Andreas Naive, Tomasz Slanina, ElSemi.
 */

#include <string.h>

#include "types.h"
#include "cps3_host.h"
#include "cps3_video.h"

#define RB_W 1024
#define RB_H 448

enum { TRANS_PEN, TRANS_PEN_INDEX, TRANS_PEN_INDEX_BLEND };

typedef struct {
    s32 l, r, t, b;
} CLIP;

static u32 renderbuffer[RB_H][RB_W];
static u32 colours[0x20000];
static s32 screen_width = 384;
static s32 screen_height = 224;

#define VREG(ofs)       cps3_be32(cps3_video_reg, ofs)
#define SPRITE_RAM32(i) cps3_be32(cps3_sprite_ram, (u32)(i) * 4)

static u32 colour_rgb(u16 data) {
    u32 r = (data & 0x1F) << 3;
    u32 g = ((data >> 5) & 0x1F) << 3;
    u32 b = ((data >> 10) & 0x1F) << 3;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* Draws a zoomed 16x16 tile into the render buffer. */
static void draw_tile16(const CLIP* clip, u32 code, u32 color, s32 granularity, s32 flipx, s32 flipy, s32 sx, s32 sy,
                        s32 transparency, s32 scalex, s32 scaley) {
    const u8* source_base;
    u32 palbase;
    s32 sw, sh, dx, dy, ex, ey, x_index_base, y_index, x, y;
    if (!scalex || !scaley) {
        return;
    }
    palbase = (granularity * color) & 0x1FFFF;
    source_base = &cps3_char_ram[(code % 0x8000) * 256];
    sh = (scaley * 16 + 0x8000) >> 16;
    sw = (scalex * 16 + 0x8000) >> 16;
    if (!sw || !sh) {
        return;
    }
    dx = (16 << 16) / sw;
    dy = (16 << 16) / sh;
    ex = sx + sw;
    ey = sy + sh;
    if (flipx) {
        x_index_base = (sw - 1) * dx;
        dx = -dx;
    } else {
        x_index_base = 0;
    }
    if (flipy) {
        y_index = (sh - 1) * dy;
        dy = -dy;
    } else {
        y_index = 0;
    }
    if (sx < clip->l) {
        x_index_base += (clip->l - sx) * dx;
        sx = clip->l;
    }
    if (sy < clip->t) {
        y_index += (clip->t - sy) * dy;
        sy = clip->t;
    }
    if (ex > clip->r + 1) {
        ex = clip->r + 1;
    }
    if (ey > clip->b + 1) {
        ey = clip->b + 1;
    }
    if (ex <= sx || ey <= sy) {
        return;
    }
    if (cps3_char_tile_pending(code % 0x8000)) {
        /* finish queued DMA for this tile first */
        cps3_chr_dma_need(code % 0x8000);
    }
    for (y = sy; y < ey; y++) {
        const u8* source = source_base + (y_index >> 16) * 16;
        u32* dest = renderbuffer[y];
        s32 x_index = x_index_base;
        for (x = sx; x < ex; x++) {
            u32 c = source[x_index >> 16];
            if (c != 0) {
                if (transparency == TRANS_PEN_INDEX) {
                    dest[x] = c | palbase;
                } else if (granularity == 64) {
                    dest[x] |= (c & 0xF) << 13;
                } else {
                    dest[x] |= ((c & 1) << 15) | ((color & 1) << 16);
                }
            }
            x_index += dx;
        }
        y_index += dy;
    }
}

static void draw_tilemapsprite_line(s32 tilemapnum, s32 drawline, const CLIP* cliprect) {
    u32 regs[4];
    s32 scrollx, scrolly, linescroll_enable, global_flip_x, global_flip_y, line, xflip_mask, tileline, tilesubline, x;
    u32 linebase, mapbase;
    CLIP clip;
    for (x = 0; x < 4; x++) {
        regs[x] = VREG(0x20 + tilemapnum * 16 + x * 4);
    }
    if (!(regs[1] & 0x8000)) {
        return;
    }
    scrollx = (regs[0] & 0xFFFF0000) >> 16;
    scrolly = regs[0] & 0xFFFF;
    linescroll_enable = (regs[1] >> 14) & 1;
    global_flip_x = (regs[1] >> 11) & 1;
    global_flip_y = (regs[1] >> 10) & 1;
    linebase = ((regs[2] & 0x7F000000) >> 24) << 10;
    mapbase = ((regs[2] & 0x007F0000) >> 16) << 10;
    scrolly += 4;
    line = (drawline + scrolly) & 0x3FF;
    if (global_flip_y) {
        line ^= 0x3FF;
    }
    xflip_mask = global_flip_x ? 0x3F : 0;
    tileline = line / 16 + 1;
    tilesubline = line % 16;
    if (linescroll_enable) {
        scrollx += (SPRITE_RAM32(linebase + ((line + 16) & 0x3FF)) >> 16) & 0x3FF;
    }
    clip.l = cliprect->l;
    clip.r = cliprect->r;
    clip.t = drawline;
    clip.b = drawline;
    for (x = cliprect->l / 16; x < cliprect->r / 16 + 2; x++) {
        u32 dat = SPRITE_RAM32(mapbase + (tileline & 63) * 64 + (((x + scrollx / 16) & 63) ^ xflip_mask));
        u32 tileno = (dat & 0xFFFE0000) >> 17;
        s32 xflip = (dat >> 12) & 1;
        s32 yflip = (dat >> 11) & 1;
        s32 alpha = (dat >> 10) & 1;
        s32 bpp = (dat >> 9) & 1;
        u32 colour = dat & 0x1FF;
        xflip ^= xflip_mask & 1;
        draw_tile16(&clip, tileno, colour, bpp ? 64 : 256, xflip, yflip, x * 16 - scrollx % 16, drawline - tilesubline,
                    alpha ? TRANS_PEN_INDEX_BLEND : TRANS_PEN_INDEX, 0x10000, 0x10000);
    }
}

static void draw_sprites(const CLIP* rclip) {
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
            if (xsize2 == 0) {
                /* tilemap, drawn line by line */
                s32 tilemapnum = (value3 >> 4) & 3;
                s32 yy;
                for (yy = 0; yy < (s32)ysizedraw2; yy++) {
                    s32 cury_pos = ypos2 + gscrolly - yy;
                    cury_pos = ~cury_pos;
                    cury_pos -= 18;
                    cury_pos &= 0x3FF;
                    if (cury_pos >= rclip->t && cury_pos <= rclip->b) {
                        draw_tilemapsprite_line(tilemapnum, cury_pos, rclip);
                    }
                }
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
                        draw_tile16(rclip, tileno + count, actualpal, granularity, flipx, flipy, current_xpos, current_ypos,
                                    trans, xscale, yscale);
                        count++;
                    }
                }
            }
        }
    }
}

/* SS text layer: 8x8 tiles, 4 bits per pixel, at 0x4000.
   Byte n of SS RAM is the low byte of u16 n. */
static u8 ss_byte(u32 n) {
    return (u8)cps3_ss_ram[n & 0x7FFF];
}

static void draw_tile8(u32* frame, s32 pitch, s32 line, u32 tile, u32 pal, s32 flipx, s32 flipy, s32 sx, s32 sy) {
    s32 row = line - sy;
    s32 x;
    u32 base;
    if (row < 0 || row > 7) {
        return;
    }
    if (flipy) {
        row = 7 - row;
    }
    base = 0x4000 + (tile & 0x1FF) * 32 + row * 4;
    for (x = 0; x < 8; x++) {
        s32 px = flipx ? 7 - x : x;
        u8 b = ss_byte(base + px / 2);
        u32 c = (px & 1) ? (b >> 4) : (b & 0xF);
        s32 dx = sx + x;
        if (c != 0 && dx >= 0 && dx < screen_width) {
            frame[line * pitch + dx] = colours[(pal * 16 + c) & 0x1FFFF];
        }
    }
}

static void draw_fg_layer(u32* frame, s32 pitch) {
    u16 vscroll = (u16)(((u8)cps3_ss_reg[0x11] << 8) | (u8)cps3_ss_reg[0x10]);
    u32 pal_base = (u8)cps3_ss_reg[0x12];
    s32 scrolly = (-vscroll) & 0x100;
    s32 line;
    for (line = 0; line < screen_height; line++) {
        s32 y = line / 8;
        s32 offset = ((line + scrolly) / 8 * 128) & 0x1FFF;
        s32 rowscroll = ss_byte(((line + scrolly - 1) & 0x1FF) * 2 + 0x2000);
        s32 x;
        for (x = 0; x < 64; x++) {
            u16 data = ss_byte(offset) | (ss_byte(offset + 1) << 8);
            u32 tile = data & 0x1FF;
            u32 pal = ((data & 0x3E00) >> 9) + (pal_base << 5);
            s32 flipy = (data >> 14) & 1;
            s32 flipx = (data >> 15) & 1;
            draw_tile8(frame, pitch, line, tile, pal, flipx, flipy, x * 8 - rowscroll, y * 8);
            draw_tile8(frame, pitch, line, tile, pal, flipx, flipy, 512 + x * 8 - rowscroll, y * 8);
            offset += 2;
        }
    }
}

void cps3_video_size(s32* width, s32* height) {
    s32 w = (s32)((VREG(0x64) & 0xFFFF0000) >> 16) - (s32)(VREG(0x60) & 0xFFFF);
    s32 h = (s32)((VREG(0x74) & 0xFFFF0000) >> 16) - (s32)(VREG(0x70) & 0xFFFF);
    if (w > 0 && w <= CPS3_VIDEO_MAX_W) {
        screen_width = w;
    }
    if (h > 0 && h <= CPS3_VIDEO_MAX_H) {
        screen_height = h;
    }
    *width = screen_width;
    *height = screen_height;
}

void cps3_video_render(u32* frame, s32 pitch) {
    u32 zx = VREG(0x6C) & 0xFF;
    u32 zy = VREG(0x7C) & 0xFF;
    u32 fszx, fszy;
    CLIP rclip;
    s32 x, y, w, h;

    cps3_video_size(&w, &h);
    for (x = 0; x < 0x20000; x++) {
        colours[x] = colour_rgb(cps3_color_ram[x]);
    }
    if (zx > 0x80) {
        zx = 0x80;
    }
    if (zy > 0x80) {
        zy = 0x80;
    }
    fszx = (zx << 16) / 0x40;
    fszy = (zy << 16) / 0x40;
    if (zx == 0x40 && zy == 0x40) {
        rclip.l = 0;
        rclip.r = w - 1;
        rclip.t = 0;
        rclip.b = h - 1;
    } else {
        rclip.l = 0;
        rclip.r = (((u32)w * fszx + 0x8000) >> 16) - 1;
        rclip.t = 0;
        rclip.b = (((u32)h * fszy + 0x8000) >> 16) - 1;
    }
    if (rclip.r >= RB_W) {
        rclip.r = RB_W - 1;
    }
    if (rclip.b >= RB_H) {
        rclip.b = RB_H - 1;
    }
    for (y = rclip.t; y <= rclip.b; y++) {
        memset(&renderbuffer[y][rclip.l], 0, (rclip.r - rclip.l + 1) * sizeof(u32));
    }

    draw_sprites(&rclip);

    for (y = 0; y < h; y++) {
        u32 sy = (zx == 0x40 && zy == 0x40) ? (u32)y : ((u32)y * fszy) >> 16;
        u32 srcx = 0;
        for (x = 0; x < w; x++) {
            u32 sx = (zx == 0x40 && zy == 0x40) ? (u32)x : srcx >> 16;
            frame[y * pitch + x] = colours[renderbuffer[sy][sx] & 0x1FFFF];
            srcx += fszx;
        }
    }

    draw_fg_layer(frame, pitch);
}

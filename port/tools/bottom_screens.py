#!/usr/bin/env python3
"""Converts 320x240 pictures into the 3DS bottom screens (port/ctr/ctr_bottom.c).

    bottom_screens.py <picture dir> [<out dir>]

Reads "<fighter> bottom screen.png" (or .jpg) for each fighter and "bottom.png" (no fighter chosen), and writes
<out dir>/<name>.z (default port/ctr/romfs/bottom): zlib-compressed RGB565 in the framebuffer's rotated order.
"""
import os
import struct
import sys
import zlib

from PIL import Image

W, H = 320, 240

FIGHTERS = ["gill", "alex", "ryu", "yun", "dudley", "necro", "hugo", "ibuki", "elena", "oro", "yang", "ken",
            "sean", "urien", "akuma", "chunli", "makoto", "q", "twelve", "remy"]


def convert(path):
    im = Image.open(path).convert("RGB")
    if im.size != (W, H):
        raise SystemExit("%s: %dx%d, not %dx%d" % (path, im.size[0], im.size[1], W, H))
    px = im.load()
    out = bytearray(W * H * 2)
    i = 0
    for x in range(W):
        for y in range(H - 1, -1, -1):
            r, g, b = px[x, y]
            v = ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)
            struct.pack_into("<H", out, i, v)
            i += 2
    return zlib.compress(bytes(out), 9)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "ctr", "romfs", "bottom")
    os.makedirs(dst, exist_ok=True)
    jobs = [("default", os.path.join(src, "bottom.png"))]
    for f in FIGHTERS:
        png = os.path.join(src, f + " bottom screen.png")
        jobs.append((f, png if os.path.exists(png) else os.path.join(src, f + " bottom screen.jpg")))
    total = 0
    for name, path in jobs:
        if not os.path.exists(path):
            print("missing:", path)
            continue
        data = convert(path)
        with open(os.path.join(dst, name + ".z"), "wb") as f:
            f.write(data)
        total += len(data)
        print("%-8s %6d bytes  (%s)" % (name, len(data), os.path.basename(path)))
    print("total %d KB" % (total // 1024))


if __name__ == "__main__":
    main()

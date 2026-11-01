# Notices

## No game data

This repository holds source code only. The game's graphics, sound and program data come from a ROM set the
user owns, converted on the user's machine by `port/tools/prepare_rom.py`.

## games/sf3

`games/sf3` is a copy of [sfIII3-cps3-decomp](https://github.com/gibletto/sfIII3-cps3-decomp) (AGPL-3.0),
edited for the port. Every change is under `HOST_PORT` / `HOST_LITTLE_ENDIAN`. The original compiler's
binaries (`bin/`) are left out.

Street Fighter, Street Fighter III 3rd Strike and Capcom are trademarks of Capcom. This project is not
affiliated with or endorsed by Capcom.

## 3DS bottom screens

The pictures in `port/ctr/romfs/bottom` show Capcom's characters, with move lists taken from
[Supercombo.gg](https://wiki.supercombo.gg/). They are not covered by this repository's license.

## MAME (BSD-3-Clause)

`port/cps3/cps3_video.c`, `port/cps3/cps3_video_dma.c` and the sprite walk in `port/ctr/ctr_video.c` are derived
from MAME's CPS3 driver (`src/mame/capcom/cps3.cpp`), and `port/cps3/cps3_sound.c` follows its sound device
(`src/mame/capcom/cps3_a.cpp`):

    license: BSD-3-Clause
    copyright-holders: David Haywood, Andreas Naive, Tomasz Slanina, ElSemi

    Redistribution and use in source and binary forms, with or without modification, are permitted
    provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice, this list of conditions
       and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
       and the following disclaimer in the documentation and/or other materials provided with the
       distribution.
    3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse
       or promote products derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
    IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
    FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
    CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
    IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
    THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

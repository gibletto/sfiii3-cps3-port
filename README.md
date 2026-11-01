# sfiii3-cps3-port

**AI Disclosure**: This used Claude. This is a barebones port of the CPS3 version of Street Fighter III
3rd Strike (990512) to PC and 3DS, built on the upstream decompilation,
**[sfIII3-cps3-decomp](https://github.com/gibletto/sfIII3-cps3-decomp)** (copied here into `games/sf3`).

It is not an emulator: the decompiled arcade program is compiled for the host, and the port supplies what the
arcade board did (memory, the task scheduler, the video and sound chips) and a front end for each platform.

**No game data is included.** You need your own copy of the arcade ROM set (`sfiii3nr1.zip`, MAME non-merged).

- [The 3DS version](3DS.md): what it does and how it runs on an original 3DS.
- [Releases](https://github.com/gibletto/sfiii3-cps3-port/releases): ready-to-use Windows and 3DS builds.

## The ROM set

Both versions take `sfiii3nr1.zip` as it is. The PC version reads it from `roms` next to `sf3.exe`. The 3DS
version reads it from `sdmc:/sf3/` and, the first time it runs, unpacks it into `sdmc:/sf3/rom-data`, because
it streams the graphics from the SD card.

## PC (Windows, SDL2)

Build with MSYS2's **mingw32** environment (32-bit, because the game stores addresses in 32 bits). You need
`mingw-w64-i686-gcc`, `mingw-w64-i686-cmake`, `mingw-w64-i686-ninja`, `mingw-w64-i686-SDL2`,
`mingw-w64-i686-zlib` and Python 3.

    cmake -G Ninja -B build -DCMAKE_C_COMPILER=gcc
    ninja -C build
    build/sf3 <sfiii3nr1.zip>

With no argument it looks for `roms/sfiii3nr1.zip` next to `sf3.exe`, then for a `rom-data` folder (the files
the 3DS unpacks). A folder argument works too.

| Keys | |
|---|---|
| Arrows | move |
| A S D | light, medium, heavy punch |
| Z X C | light, medium, heavy kick |
| 1 / 5 | start / coin (2 / 6 for player 2) |
| F2 | test menu (settings, free play) |
| 9 | service coin |
| Alt+Enter, F11 | full screen |
| F12 | screenshot (frame.bmp) |
| Escape | quit |

Game controllers work too: D-pad or left stick, X Y RB punches, A B RT kicks, Start, and Back for a coin.

## Nintendo 3DS

Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group and `3ds-zlib`. Any
CMake works:

    cmake -G Ninja -B build-3ds -DCMAKE_TOOLCHAIN_FILE=port/cmake/3ds-native.cmake
    ninja -C build-3ds

This builds `build-3ds/sf3.3dsx`. If [makerom](https://github.com/3DSGuy/Project_CTR) and
[bannertool](https://github.com/diasurgical/bannertool) are on the PATH (or given with `-DCTR_MAKEROM=` and
`-DCTR_BANNERTOOL=`), it also builds `build-3ds/sf3.cia`, an installable title that uses the Old 3DS's 80 MB
memory mode. See the README in a [release](https://github.com/gibletto/sfiii3-cps3-port/releases) for how to set
up the SD card.

| 3DS buttons | |
|---|---|
| D-pad / Circle Pad | move |
| Y X L | light, medium, heavy punch |
| B A R | light, medium, heavy kick |
| ZL / ZR (New 3DS) | all three punches / all three kicks |
| START | start |
| SELECT | insert a coin |
| SELECT + START | operator (test) menu: settings, free play, difficulty and so on |
| SELECT + L | service coin (a free credit) |
| SELECT + R | button setup |
| SELECT + B | quit |

The attack buttons can be changed. SELECT + R opens a setup screen on the bottom screen that asks for a button
for each attack, or edit `sdmc:/sf3/controls.ini`, which is made the first time the game runs. In the file a
button can press several at once, for example `R = HP HK`.

In the operator menu, choose CONFIGURATION to change the game settings and coin settings, including free play.
Settings are saved like on the arcade board.

Publishing a GitHub release runs `.github/workflows/release.yml`. It builds the 3DS version with the devkitPro
Docker image and the Windows version with MSYS2, and attaches both zips to the release.

## Layout

    games/sf3/      the decompiled game (a downstream copy; port changes are under HOST_PORT)
    port/cps3/      the arcade board: memory, kernel, video, sound, EEPROM
    port/sys/       coroutines and division by zero, per platform
    port/sdl/       PC front end
    port/ctr/       3DS front end and GPU renderer
    port/tools/     build helpers

## License

AGPL-3.0 (see `LICENSE`), as for the decompilation. Some files are derived from MAME (BSD-3-Clause); see
`NOTICE.md`. Street Fighter and Capcom are trademarks of Capcom; this project is not affiliated with or endorsed
by Capcom.

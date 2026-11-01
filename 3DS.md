# Street Fighter III 3rd Strike on the 3DS

This port is built for the **original 3DS and 2DS**, the slowest models. On a New 3DS it also switches on the
faster CPU mode.

It isn't an emulator. The arcade game's own code is compiled to run directly on the 3DS's processor. An
emulator would have to imitate the arcade's CPU instruction by instruction, which an original 3DS is far too
slow to do.

## What you get

- The arcade game, with its attract mode, test menu and settings.
- **Bottom screen:** once you pick your fighter, it shows their move list.
- **3D:** with the 3D slider up, the start-up text, the CAPCOM logo, the title screen and the demo's logo and
  INSERT COIN stand out in depth. The fights themselves stay flat.
- **Sound** through the 3DS's sound chip (needs the DSP firmware; see the release README).

## Controls

| Button | |
|---|---|
| D-pad / Circle Pad | move |
| Y X L | light, medium, heavy punch |
| B A R | light, medium, heavy kick |
| START | start |
| SELECT | insert a coin |
| SELECT + START | operator (test) menu |
| SELECT + L | service coin (a free credit) |
| SELECT + B | quit |

Hold SELECT and press START to open the arcade's operator menu. Go to CONFIGURATION to change the settings:
difficulty, round time, coins per credit, and **free play**. The settings are saved to the SD card like on the
arcade board.

## How it fits on an original 3DS

An original 3DS has a 268 MHz processor and 64 MB of memory for the game. The arcade game needs more than that.

**Memory.** The game's graphics alone take 64 MB. The port keeps about 30 MB of them in memory and reads the
rest from the SD card as needed. A background thread guesses what comes next and reads it early, so the game
rarely waits for the card.

**Graphics.** This is where most of the work went.

The arcade draws everything with *palettes*: each pixel stores a colour number, and a small colour table turns
the numbers into colours. The game swaps these tables constantly, for example to flash a character when hit.
The 3DS's graphics chip (the PICA200) can't use palettes at all.

Converting the pixels to real colours on the processor every frame was far too slow. Instead, the port borrows
a part of the graphics chip meant for something else: **the lighting hardware**. It has small lookup tables for
how light falls on a surface. The port loads the arcade's colour table into those, and makes each pixel's colour
number act as the "angle of the light". The lighting hardware then looks up the colour, so the graphics chip
does the palette work for free. Changing a palette is just loading a small table again.

A few more tricks keep the processor's work small:

- **Geometry shader.** Each sprite goes to the graphics chip as one small record. A *geometry shader* (a little
  program on the graphics chip) turns it into the rectangle on screen. Building every rectangle's corners on
  the processor cost more time than the rest of the frame.
- **Grouped drawing.** Sprites are drawn grouped by palette, so each table is loaded once. The depth buffer
  keeps them in the arcade's front-to-back order.
- **Shadows** are marked in the stencil buffer, and the pixels under them are redrawn with the darker palette.
- **Only what changed.** Graphics are copied to the graphics chip only when the game changes them.
- **Loading in spare time.** Between scenes the game unpacks a lot of graphics at once. The port spreads that
  work into the time each frame spends waiting for the screen to refresh.

## Building it yourself

See the main [README](README.md). Releases are built automatically from this repository.

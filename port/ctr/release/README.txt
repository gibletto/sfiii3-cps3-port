Street Fighter III 3rd Strike for Nintendo 3DS
==============================================

A native port of the arcade game (not an emulator), built for the original 3DS and 2DS. It runs on a New 3DS
too. This zip contains NO game data: you need your own copy of the arcade ROM set.

https://github.com/gibletto/sfiii3-cps3-port

What you need
-------------
- A 3DS with custom firmware (Luma3DS).
- The arcade ROM set as a MAME zip, sfiii3nr1.zip (non-merged).
- About 140 MB free on the SD card (70 MB once the zip is deleted).

1. Copy to the SD card
----------------------
Copy the contents of the sd folder to the root of the SD card, and put sfiii3nr1.zip in sdmc:/sf3/ as it is.

Then either:
- install sf3.cia with FBI (recommended): the game gets its own HOME Menu icon, and on an original 3DS it uses
  the larger 80 MB memory mode, so it reads the SD card less; or
- use sdmc:/3ds/sf3.3dsx from the Homebrew Launcher.

Sound needs the 3DS's DSP firmware, which can't be included. Run the DSP1 homebrew once: it saves it as
sdmc:/3ds/dspfirm.cdc. Without it the game runs silent.

2. Play
-------
Start "SF III 3rd Strike". The first time, it unpacks sfiii3nr1.zip into sdmc:/sf3/rom-data ("One-time ROM
unpack", about a minute). After that you can delete the zip.

    D-pad / Circle Pad   move
    Y  X  L              light / medium / heavy punch
    B  A  R              light / medium / heavy kick
    ZL / ZR              all three punches / all three kicks (New 3DS)
    START                start
    SELECT               insert a coin
    SELECT + START       operator (test) menu: settings, free play
    SELECT + L           service coin (a free credit)
    SELECT + R           button setup
    SELECT + B           quit

Changing the buttons: hold SELECT and press R. The game pauses and the bottom screen asks you to press the
button for each attack in turn (SELECT for none, START to cancel). The buttons are saved in
sdmc:/sf3/controls.ini, which you can also edit on a PC: each line is a button and what it does, and a button
can do several things at once, for example "R = HP HK".

In the operator menu, CONFIGURATION has the game and coin settings, including free play. Settings and high
scores are saved in sdmc:/sf3/rom-data/eeprom.bin.

The bottom screen shows your fighter's move list once you have chosen them (move lists from Supercombo.gg).
With the 3D slider up, the attract screens have depth.

Street Fighter and Capcom are trademarks of Capcom. This project is not affiliated with or endorsed by Capcom.

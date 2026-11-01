Street Fighter III 3rd Strike for Nintendo 3DS
==============================================

A native port of the arcade game (not an emulator), built for the original 3DS and 2DS. It runs on a New 3DS
too. This zip contains NO game data: you need your own copy of the arcade ROM set.

https://github.com/gibletto/sfiii3-cps3-port

What you need
-------------
- A 3DS with custom firmware (Luma3DS) and the Homebrew Launcher.
- The arcade ROM set as a MAME zip, sfiii3nr1.zip (non-merged).
- Python 3 on a PC, to convert the ROM set.

1. Convert the ROM set (on the PC)
----------------------------------
    python tools/prepare_rom.py <path to sfiii3nr1.zip> rom-data

This writes bios.bin, cg.bin and user5.bin into rom-data.

2. Copy to the SD card
----------------------
Copy the contents of the sd folder to the root of the SD card, then put the three files from step 1 in
sdmc:/sf3/rom-data/:

    sdmc:/sf3/rom-data/bios.bin
    sdmc:/sf3/rom-data/cg.bin
    sdmc:/sf3/rom-data/user5.bin

Then either install the CIA or use the 3dsx:

- sf3.cia (recommended): copy it to the SD card and install it with FBI. The game gets its own icon on the
  HOME Menu. On an original 3DS it asks for the larger 80 MB memory mode, so it keeps more of the graphics in
  memory and reads the SD card less.
- sdmc:/3ds/sf3.3dsx: start it from the Homebrew Launcher. Nothing to install.

Sound needs the 3DS's DSP firmware, which can't be included. Run the DSP1 homebrew once: it saves it as
sdmc:/3ds/dspfirm.cdc. Without it the game runs silent.

3. Play
-------
Start "SF III 3rd Strike" from the HOME Menu (CIA) or the Homebrew Launcher (3dsx). It takes a few seconds to
load.

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

Hold SELECT and press START for the arcade's operator menu. CONFIGURATION has the game and coin settings,
including free play. Settings and high scores are saved in sdmc:/sf3/rom-data/eeprom.bin.

The bottom screen shows your fighter's move list once you have chosen them (move lists from Supercombo.gg).
With the 3D slider up, the attract screens have depth.

Street Fighter and Capcom are trademarks of Capcom. This project is not affiliated with or endorsed by Capcom.

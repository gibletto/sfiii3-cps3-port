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

    sdmc:/3ds/sf3.3dsx
    sdmc:/sf3/rom-data/bios.bin
    sdmc:/sf3/rom-data/cg.bin
    sdmc:/sf3/rom-data/user5.bin

Sound needs the 3DS's DSP firmware, which can't be included. Run the DSP1 homebrew once: it saves it as
sdmc:/3ds/dspfirm.cdc. Without it the game runs silent.

3. Play
-------
Start "SF III 3rd Strike" from the Homebrew Launcher. It takes a few seconds to load.

    D-pad / Circle Pad   move
    Y  X  L              light / medium / heavy punch
    B  A  R              light / medium / heavy kick
    START                start
    SELECT               insert a coin
    SELECT + START       operator (test) menu: settings, free play
    SELECT + L           service coin (a free credit)
    SELECT + B           quit

Hold SELECT and press START for the arcade's operator menu. CONFIGURATION has the game and coin settings,
including free play. Settings and high scores are saved in sdmc:/sf3/rom-data/eeprom.bin.

The bottom screen shows your fighter's move list once you have chosen them (move lists from Supercombo.gg).
With the 3D slider up, the attract screens have depth.

Street Fighter and Capcom are trademarks of Capcom. This project is not affiliated with or endorsed by Capcom.

SSX 3 for Nintendo Switch (SSX3NX)
==================================

A native Switch port of SSX 3 (PAL Xbox version), built from a static
recompilation of the game. It runs as homebrew (.nro) and holds a steady
30 FPS in races at the Switch's stock clocks, with 60 FPS menus.

This package does NOT contain the game's data. You need your own copy
of the PAL Xbox disc.


WHAT YOU NEED
-------------
- A Switch running custom firmware (Atmosphere) with the Homebrew Menu.
- Your own dump of the PAL Xbox SSX 3 disc: default.xbe and the data folder.
- About 7.5 GB free on the SD card: ~2.1 GB for the game files, plus ~5.2 GB
  of emulated Xbox hard disk that the game creates the first time it starts.


INSTALLING
----------
1. Copy the "switch" folder from this zip to the root of your SD card.
   You should end up with:

     sdmc:/switch/ssx3nx/SSX3NX.nro
     sdmc:/switch/ssx3nx/SSX 3 env.txt
     sdmc:/switch/ssx3nx/README.txt

2. Copy your disc dump into a "game" folder next to it:

     sdmc:/switch/ssx3nx/game/default.xbe
     sdmc:/switch/ssx3nx/game/data/...

3. Start the Homebrew Menu by opening a game while holding R
   (title override). Don't use the Album: homebrew started from there
   doesn't get enough memory to run the game.

4. Pick SSX 3 (SSX3NX). The first start takes a little longer while the
   emulated hard disk (Partition0-5.img) is created.


CONTROLS
--------
Riding uses the PS2 layout by default:

  B          jump
  Y          boost / tweak
  A          handplant
  L R ZL ZR  the four grabs
  X (or -)   reset

Menus use the Nintendo buttons: A to confirm, B to go back.

The on-screen button prompts, the Options > Controller page and the
loading screens all show the Switch buttons. To use the Xbox layout
instead, set SSX3NX_PS2_CONTROLS=0 in the settings file.


SETTINGS
--------
"SSX 3 env.txt" holds every setting, with an explanation next to each one.
The defaults are the tested ones; you don't need to change anything. Popular
changes:

- Skip the long intro movie:     RECOMP_SKIP_INTRO=2
- Speed lines without the blur:  SSX3NX_BOOST_LINES=2
- Show the frame rate:           remove the '#' in front of RECOMP_FPS_OVERLAY=1
- Switch Lite, sharper picture:  remove the '#' in front of SSX3NX_DOCKED=1

The game sets the stock clocks itself while it runs (CPU 1020, GPU 384,
memory 1331 MHz), even if sys-clk is installed. The settings file lists the
overclock values for anyone who wants extra headroom.


SAVES
-----
Your saves are stored inside the emulated hard disk, the Partition*.img
files in sdmc:/switch/ssx3nx/.

  Deleting those files deletes your saves.

Back them up if you reinstall. When updating to a new version, replace
only SSX3NX.nro (and the settings file if you want the new defaults).


IF SOMETHING GOES WRONG
-----------------------
- "default.xbe was not found": check the game folder layout in step 2.
- Black screen, broken textures, odd geometry or crackling audio: section 6
  (Troubleshooting) of the settings file has one switch per symptom.
- To report a bug: set RECOMP_LOG_LEVEL=info in the settings file,
  reproduce the problem, and include sdmc:/switch/ssx3nx/SSX3NX.log.


KNOWN ISSUES
------------
- The first few seconds after an event loads (the pre-race flyover) run at
  about 27-28 FPS. The race itself holds 30.
- Character select once showed some flickering textures; it hasn't been
  seen again. If you see it, try SSX3NX_NATIVE_RENDER=0 in the settings file.

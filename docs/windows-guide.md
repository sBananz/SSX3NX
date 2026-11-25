# Building SSX3NX on Windows, step by step

This guide is for people who have never built anything before. The **SSX3NX Builder** window installs what's needed,
builds the game from your own copy of SSX 3 and copies it to your SD card. You press four buttons.

You need:

- A Windows 10 or 11 PC with about **15 GB free** and an internet connection. 16 GB of memory is comfortable; 8 GB
  works, more slowly.
- Your own ***SSX 3* for the original Xbox, PAL (European) release**: either the game's files (a folder with
  `default.xbe` and `data` in it) or a disc image (`.iso`). Other versions won't work, and the builder checks.
- A Switch with custom firmware (Atmosphère) and the Homebrew Menu, its SD card, and a card reader for the PC.
  The SD card needs about **7.5 GB free**.

Nothing of the game is downloaded: the builder makes it from your copy, so the result is yours alone. Don't share it.

## 1. Download SSX3NX

1. Open the [latest release](https://github.com/sBananz/SSX3NX/releases/latest) and click **Source code (zip)** at the
   bottom.
2. Right-click the downloaded zip, choose **Extract All...**, and extract it to a short folder such as `C:\SSX3NX`.
   Avoid OneDrive, Desktop and Downloads folders: the build writes many files.
3. Open that folder. You should see `SSX3NX Builder.bat` among the files.

## 2. Start the builder

Double-click **`SSX3NX Builder.bat`**. If Windows says *"Windows protected your PC"*, click **More info** and then
**Run anyway**. The SSX3NX Builder window opens.

The window has four numbered parts, top to bottom. Do them in order.

## 3. Install the tools (once)

Press **Install / check**. The builder downloads and sets up:

| | What it is | Where it goes |
|---|---|---|
| **MSYS2** | Python and small tools that translate the game's code | `C:\msys64` |
| **Docker Desktop** | Runs the Switch compiler | Installed like any program |
| **The Switch compiler** | devkitPro's `devkitpro/devkita64`, about 3 GB | Inside Docker |
| **Vulkan driver files** | Mesa NVK, the Switch graphics driver the game uses | `build\windows\nvk-sdk` in the SSX3NX folder |
| **The recompiler** | xboxrecomp, with SSX 3's and the Switch's changes | `build\xboxrecomp` in the SSX3NX folder |

Along the way:

- Windows asks **twice for permission** (to turn on WSL and to install Docker Desktop). Click **Yes**.
- After Docker Desktop is installed, the builder asks you to **restart the computer**. Do that, start the builder again
  and press **Install / check** once more.
- Docker Desktop opens a window the first time. **Accept** its terms; you can **skip** signing in and the survey.
  Leave Docker Desktop running while you build (it can stay in the taskbar corner).

When all four lines at the top say **[OK]**, the tools are ready. You never have to do this part again.

## 4. Pick your game

Under **2. Your game**, press:

- **Folder...** if you have the game's files: pick the folder that holds `default.xbe` and the `data` folder, or
- **Disc image...** if you have an `.iso` of your disc. It's unpacked once when you build (about 2 GB).

## 5. Build

Press **Build SSX3NX.nro**. The first build takes **20 to 40 minutes**. The messages show what it's doing, and the line
at the bottom shows the step. You can use the computer for other things meanwhile. Later builds take a few minutes.

When it's done, the bottom line says **"SSX3NX.nro is built"**.

If the builder says your `default.xbe` is not the PAL release, your copy is a different version (USA, Japanese, or a
different disc). SSX3NX only works with the PAL Xbox release.

## 6. Copy to the SD card

1. Put the Switch's SD card in the computer.
2. Under **4. Copy to the SD card**, press **Refresh** and pick the card's drive letter.
3. Press **Copy to SD card**. It copies the game program, the settings file and your game files (about 2 GB, several
   minutes) to `switch\ssx3nx\` on the card. Your settings and game files already on the card are kept.

## 7. Play

1. Put the SD card back in the Switch.
2. Hold **R** while starting any game you have installed. The Homebrew Menu opens **over the game**. Starting it from
   the Album doesn't give homebrew enough memory.
3. Pick **SSX 3 (SSX3NX)**.
4. **The first start takes a few minutes longer**: the game creates its emulated Xbox hard disk (about 5.2 GB on the
   card). After that it starts quickly.

Your saves live in that hard disk (the `Partition*.img` files on the card): don't delete them.

## Settings

`switch\ssx3nx\SSX 3 env.txt` on the SD card holds every setting with an explanation, for example skipping the long
intro movie (`RECOMP_SKIP_INTRO=2`) or showing the frame rate (`RECOMP_FPS_OVERLAY=1`). Edit it with Notepad.

## If something goes wrong

- The window shows a message saying what stopped, and the full messages are in `build\windows\` (`setup.log`,
  `build.log`, `sdcard.log`).
- **Docker Desktop did not start**: open it from the Start menu, wait until it says it's running, then press the button
  again.
- **The build stops with "Killed" or stalls**: the PC is short of memory. Close other programs and press Build again.
- **A problem with the game on the Switch**: see Troubleshooting in the [README](../README.md#troubleshooting).
- Still stuck? Open an [issue](https://github.com/sBananz/SSX3NX/issues) and attach the log from `build\windows\`.

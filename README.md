<h1 align="center">SSX3NX</h1>

<p align="center">
  <b>SSX 3 on the Nintendo Switch, natively.</b><br>
  The original Xbox game, recompiled for the Switch: a steady 30 FPS on the console's stock clocks,
  widescreen, and Switch controls throughout.
</p>

<p align="center">
  <img alt="Status: playable" src="https://img.shields.io/badge/status-playable-brightgreen">
  <img alt="Platform: Nintendo Switch homebrew" src="https://img.shields.io/badge/platform-Nintendo%20Switch%20(homebrew)-e60012">
  <img alt="Graphics: Vulkan (NVK)" src="https://img.shields.io/badge/graphics-Vulkan%20(NVK)-blue">
  <img alt="Races: 30 fps" src="https://img.shields.io/badge/races-30%20fps%20%C2%B7%20stock%20clocks-brightgreen">
  <img alt="Menus: 60 fps" src="https://img.shields.io/badge/menus-60%20fps-brightgreen">
  <a href="LICENSE"><img alt="License: GPL-3.0-only" src="https://img.shields.io/badge/license-GPL--3.0--only-green"></a>
</p>

> [!IMPORTANT]
> **You need your own game.** No disc image, game data, game code or ready-made `.nro` is distributed here, and none will be.
> You build `SSX3NX.nro` yourself from your own copy of ***SSX 3* for the original Xbox, PAL (European) release**.
> The build contains code translated from your copy of the game, so keep it to yourself.

---

## Contents

- [What is this?](#what-is-this)
- [Features](#features)
- [Quick start (Windows)](#quick-start-windows)
- [What you need](#what-you-need)
- [Build](#build)
- [Install and play](#install-and-play)
- [Controls](#controls)
- [Settings](#settings)
- [Troubleshooting](#troubleshooting)
- [Known issues](#known-issues)
- [Credits and acknowledgments](#credits-and-acknowledgments)
- [Legal](#legal)

---

## What is this?

SSX3NX runs *SSX 3* (2003) natively on the Nintendo Switch. It is **not an emulator**: the game's own Xbox code is translated to C
with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and compiled for the Switch's ARM CPU. The Xbox's graphics, sound and
controller hardware are reimplemented on top of Vulkan (Mesa NVK), `audout` and `hid`, through devkitPro's libnx.

It is the Switch port of [**SSX3-recomp**](https://github.com/koenhendriks/SSX3-recomp), Koen Hendriks' PC recompilation of the game,
with the Switch platform layer brought over from [**OpenTrickyNX**](https://github.com/DevCrashDay/OpenTrickyNX), the Switch edition of
[OpenTricky](https://github.com/GiZcesi/OpenTricky).

## Features

- **30 FPS races on stock handheld clocks** (CPU 1020 / GPU 384 / memory 1331 MHz), with **60 FPS menus**. The game's logic,
  physics and controls keep running at their original 60 Hz underneath; only the drawing is paced to 30, using the game's own
  half-rate mode. In testing, Metro City (the busiest race) averaged 29.8 FPS.
- **Dynamic draw distance**: in the rare frames that would run late, the view pulls in slightly and eases back out, instead of
  dropping a frame.
- **Widescreen**: 3D through a true 16:9 camera, and menus stretched evenly to 16:9 (or kept 4:3 with side bars, if you prefer).
- **Docked at 1080p**: switches resolution and clocks when you dock or undock, even mid-race. A Switch Lite can use the docked
  mode too, for a sharper picture.
- **Switch controls throughout**:
  - **PS2-style riding layout** by default (B jump, Y boost, A handplant, L/R/ZL/ZR grabs), or the Xbox layout by position.
  - **Nintendo buttons in menus**: A confirm, B back.
  - **Switch button icons** in every on-screen prompt.
  - **A Pro Controller** on the Options › Controller page and the loading screens, with the callouts re-laid for both
    controller configs. You can supply your own controller picture.
  - **Switch wording and logo** on the saving screens.
- **Boost speed lines** (optional, on by default): speed lines and a soft blur at the screen edges while you boost, in the spirit
  of *SSX* (2012) and *SSX On Tour*. Lines only, or off, in the settings.
- **The rest of the game as it was**: saves, music and speech, and every race and event tested so far.
- **One settings file** (`SSX 3 env.txt`) with every option explained, and a troubleshooting section that ties each switch to a
  symptom.

## Quick start (Windows)

1. Download **Source code (zip)** from the [latest release](https://github.com/sBananz/SSX3NX/releases/latest) and extract it
   to a short folder like `C:\SSX3NX`.
2. Double-click **`SSX3NX Builder.bat`**.
3. Press the four buttons in order: **Install / check** (once), pick your game (**Folder...** or **Disc image...**),
   **Build SSX3NX.nro**, then **Copy to SD card**.

The builder installs everything it needs, checks that your copy is the PAL release, builds the game from it and copies
it to your SD card. The first build takes 20 to 40 minutes. **[Step-by-step guide with every click explained](docs/windows-guide.md)**

On Linux or macOS, or to build by hand, see [Build](#build).

## What you need

| | |
|---|---|
| **A computer** | Windows, Linux or macOS with **Docker** (Docker Desktop on Windows and macOS, started before you build). It provides the Switch compiler: devkitPro's `devkitpro/devkita64` image, about 3 GB, downloaded on the first build. An installed devkitPro works too (`DEVKITPRO=...`). |
| **On Windows** | [MSYS2](https://www.msys2.org/). In its **UCRT64** shell: `pacman -S --needed git mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone` |
| **On Linux or macOS** | Git, Python 3 and `capstone` (`scripts/setup.sh` sets up a virtual environment with it). |
| **For Vulkan** | The `mesa-switch-nvk-sdk` download from OpenTrickyNX's "Mesa NVK SDK" workflow ([Actions](https://github.com/DevCrashDay/OpenTrickyNX/actions)). Unzip it and keep its `portlibs/switch` folder. Without it, the build only has the (far too slow) software renderer. |
| **The game** | Your own ***SSX 3* Xbox disc, PAL release**, as files: `default.xbe` and the `data` folder (extract your disc image with [extract-xiso](https://github.com/XboxDev/extract-xiso)). Other regions and the PS2 / GameCube versions won't work. |
| **The console** | A Switch running custom firmware (Atmosphère) with the Homebrew Menu, and about **7.5 GB free** on the SD card: ~2.1 GB of game files plus ~5.2 GB of emulated Xbox hard disk, created on first boot. |

## Build

On Windows, the [SSX3NX Builder](#quick-start-windows) does all of this for you. By hand:

**1. Lay out the folders.** The scripts expect three folders side by side:

```
<anywhere>/
  SSX3NX/          this repository
  xboxrecomp/      the recompiler toolkit (step 2 creates it)
  SSX3/            your game files: default.xbe + data/
```

```bash
git clone https://github.com/sBananz/SSX3NX.git
```

The game folder can live elsewhere if you point `XBE=/path/to/default.xbe` at it.

**2. Set up the toolkit (once).** This clones xboxrecomp v0.13.1 next to the repo and applies `patches/xboxrecomp-local.patch`
(SSX 3's runtime changes) and `patches/xboxrecomp-switch.patch` (the Switch layer):

```bash
bash scripts/setup.sh
```

On Windows, run it from the MSYS2 **UCRT64** shell, in the repository folder.

**3. Build.** Start Docker first.

**Windows:** set the NVK SDK path, then run the batch file (it opens MSYS2 for you):

```powershell
$env:NVK_SDK = "C:\path\to\portlibs\switch"
.\build_switch.bat
```

**Linux or macOS:**

```bash
NVK_SDK=/path/to/portlibs/switch ./build_switch.sh
```

The script translates your `default.xbe` to C (a few minutes, only the first time) and compiles `dist/switch/ssx3nx/SSX3NX.nro`
(10 to 30 minutes the first time, then only what changed). The output folder also gets `SSX 3 env.txt` and a player `README.txt`.

| Build option | |
|---|---|
| `XBE=/path/to/default.xbe` | Your game's `default.xbe`, if it isn't in `../SSX3/`. |
| `SWITCH_JOBS=4` | Compile fewer files at once. The translated game code takes a few GB of memory per file: use this if Docker runs short of memory. |
| `SWITCH_OPTS="..."` | Extra CMake options, kept by the build folder. |
| `TK=/path/to/xboxrecomp` | The toolkit, if it isn't next to the repo. |

In PowerShell, set these as `$env:SWITCH_JOBS = "4"`.

## Install and play

1. Copy `dist/switch/ssx3nx/` to `sdmc:/switch/` on your SD card.
2. Copy your game files into a `game` folder inside it, so that `sdmc:/switch/ssx3nx/game/default.xbe` and
   `sdmc:/switch/ssx3nx/game/data/` exist.
3. Hold **R** while starting any installed game, which opens the Homebrew Menu **over a game** (title override). Then
   start **SSX 3 (SSX3NX)**. If you start it from the Album instead, homebrew doesn't get enough memory to run the game.
4. **The first start takes longer**: the game creates its emulated Xbox hard disk (`Partition0-5.img`, about 5.2 GB in total).
   SSX 3 needs these cache partitions and won't start without them.

> [!WARNING]
> Your saves live inside the `Partition*.img` files. Deleting them deletes your saves. To update, replace only `SSX3NX.nro`.

## Controls

| Riding (default, PS2 layout) | |
|---|---|
| **B** | Jump |
| **Y** | Boost / tweak |
| **A** | Handplant |
| **L R ZL ZR** | The four grabs |
| **X** or **−** | Reset |

Menus and the pause menu use the Nintendo buttons: **A** to confirm, **B** to go back. `SSX3NX_PS2_CONTROLS=0` switches riding to
the Xbox layout by position. Joy-Cons (attached or held together) and the Pro Controller all work.

## Settings

Everything lives in `sdmc:/switch/ssx3nx/`:

| | |
|---|---|
| `SSX 3 env.txt` | All the settings, one `NAME=value` per line, each with an explanation. The defaults are the tested ones. |
| `Partition0-5.img` | The emulated Xbox hard disk, including your saves. |
| `SSX3NX.log` | What the game reports, for bug reports. |
| `icons/pro_controller.png` | Optional: your own picture for the controller page. |
| `vk_pipeline.cache` | Compiled GPU pipelines, so later boots don't stutter. |

Popular changes in `SSX 3 env.txt`:

| | |
|---|---|
| `RECOMP_SKIP_INTRO=2` | EA and THX logos only, without the 3.5-minute intro movie (`1` skips all of them). |
| `SSX3NX_BOOST_LINES=2` | Boost speed lines without the edge blur (`0` = off). |
| `RECOMP_FPS_OVERLAY=1` | Show the frame rate and draw count in the corner. |
| `SSX3NX_DOCKED=1` | Act docked on a Switch Lite: docked clocks and a sharper picture. |
| `SSX3NX_DOCK_RES=900` | Docked resolution in lines (1080 by default). |
| `SSX3NX_CPU_MHZ=1785`, `SSX3NX_GPU_MHZ=768`, `SSX3NX_MEM_MHZ=1600` | Overclock for extra headroom. Costs battery and heat. |
| `RECOMP_WIDESCREEN_MENUS=0` | Menus in 4:3 with side bars, as the game drew them. |

The game sets the stock clocks itself while it runs, even with sys-clk installed (`SSX3NX_CLOCK_FORCE=1`). A `.nro` runs inside
whichever game hosts the Homebrew Menu, so a sys-clk profile made for SSX 3 never applies anyway.

## Troubleshooting

Every speed-up is on by default and has a switch back to the slower, original method. Try one at a time, in `SSX 3 env.txt`:

| Symptom | Try |
|---|---|
| Black screen, or the picture looks broken | `RECOMP_VK_PRESENT=0` |
| Textures look wrong or blocky | `RECOMP_TEX_DXT_NATIVE=0` |
| A texture shows late or wrong for a moment | `RECOMP_TEX_SAMPLED_HASH=0` |
| Geometry looks wrong (stretched or missing shapes) | `RECOMP_VSH_INDEXED=0` |
| Objects popping out, flickering textures or wrong colours | `SSX3NX_NATIVE_RENDER=0` |
| Crackling music or speech | `RECOMP_APU_STREAM_CUSHION_MS=120` |
| "default.xbe was not found" | Check the folder layout in [Install and play](#install-and-play). |
| The game says it doesn't have enough memory | Start it over a game (hold R), not from the Album. |

**Reporting a bug:** set `RECOMP_LOG_LEVEL=info`, reproduce the problem, and open an [issue](../../issues) with what you did, what
happened, and `SSX3NX.log`.

## Known issues

- **Pre-race flyover**: the first few seconds after an event loads run at about 27–28 FPS. The race itself holds 30.
- **First-time stutter**: the first time an effect appears, its GPU pipeline is compiled. They're kept in `vk_pipeline.cache`,
  so this only happens once.
- **Flickering textures at character select** were seen once and haven't reappeared. If you see them, try
  `SSX3NX_NATIVE_RENDER=0` and report it.
- **PAL only**: the translation is made for the PAL Xbox `default.xbe`.

## Other platforms

The same sources still build the PC version (Windows and Linux) from upstream SSX3-recomp. See [docs/PC-README.md](docs/PC-README.md).

## Credits and acknowledgments

SSX3NX stands on the work of these projects and people:

| Who | What |
|---|---|
| [**sp00nznet**](https://github.com/sp00nznet) | [**xboxrecomp**](https://github.com/sp00nznet/xboxrecomp), the static recompiler and Xbox runtime this whole port is built on (MIT). Also [flatout2-sw](https://github.com/sp00nznet/flatout2-sw), whose Switch clock code the clock handling follows (MIT). |
| [**DevCrashDay**](https://github.com/DevCrashDay) | [**OpenTrickyNX**](https://github.com/DevCrashDay/OpenTrickyNX), the SSX Tricky Switch port. Its Switch platform layer (libnx entry, settings file, clocks, `audout` audio, `hid` input, Mesa NVK renderer work, Switch save fixes and build scripts) was transplanted into this port (GPL-3.0-only). |
| [**GiZcesi**](https://github.com/GiZcesi) | [**OpenTricky**](https://github.com/GiZcesi/OpenTricky), the SSX Tricky PC port that OpenTrickyNX is built on. |
| [**MatiasRiveraC**](https://github.com/MatiasRiveraC) | [SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC), the recompilation work OpenTricky continues. |
| [**Koen Hendriks**](https://github.com/koenhendriks) | [**SSX3-recomp**](https://github.com/koenhendriks/SSX3-recomp), the SSX 3 PC recompilation this port starts from: the game hooks, renderer, audio and reverse engineering (GPL-2.0-or-later). |
| [**xemu**](https://xemu.app) | The Xbox GPU and audio emulation that parts of the renderer and audio are adapted from (LGPL-2.1-or-later), and the accuracy reference. |
| [**devkitPro**](https://devkitpro.org/) | devkitA64, libnx and the Switch portlibs. |
| [**Mesa**](https://mesa3d.org/) | The NVK Vulkan driver, and its Switch build. |
| [Capstone](https://www.capstone-engine.org/), [Ghidra](https://ghidra-sre.org/) | Disassembly and reverse engineering. |
| **EA Canada** | For making *SSX 3*. |

## Legal

- SSX3NX is an unofficial fan project. It is not affiliated with or endorsed by Electronic Arts, EA Canada, Microsoft or Nintendo.
  *SSX* and *SSX 3* are trademarks of Electronic Arts, Xbox is a trademark of Microsoft, and Nintendo Switch is a trademark of
  Nintendo.
- **No disc image, game data, game code, music or video is distributed.** You need your own copy of the game, and the `.nro` you
  build from it is for your own use.
- Licences: this project is distributed under the **GNU General Public License v3.0 only** (`GPL-3.0-only`, see [LICENSE](LICENSE)),
  because it includes code from OpenTrickyNX under that licence. Code from SSX3-recomp was released under GPL-2.0-or-later and is
  used here under GPL-3.0. The xboxrecomp changes in `patches/` follow xboxrecomp's own licences (MIT, and LGPL-2.1-or-later for
  the parts adapted from xemu).

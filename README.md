<h1 align="center">SSX3NX</h1>

<p align="center">
  <b>SSX 3 running natively on the Nintendo Switch.</b><br>
  30 FPS races, 60 FPS menus, widescreen and Switch controls, on the console's normal clocks.
</p>

<p align="center">
  <img alt="Status: playable" src="https://img.shields.io/badge/status-playable-brightgreen">
  <img alt="Platform: Nintendo Switch homebrew" src="https://img.shields.io/badge/platform-Nintendo%20Switch%20(homebrew)-e60012">
  <img alt="Races: 30 fps" src="https://img.shields.io/badge/races-30%20fps-brightgreen">
  <img alt="Menus: 60 fps" src="https://img.shields.io/badge/menus-60%20fps-brightgreen">
  <a href="LICENSE"><img alt="License: GPL-3.0-only" src="https://img.shields.io/badge/license-GPL--3.0--only-green"></a>
</p>

> [!IMPORTANT]
> **You need your own copy of SSX 3 for the original Xbox (PAL / European version).** No game files are included and no
> ready-made game is shared. The SSX3NX Builder makes the game from your copy, so what you build is yours alone: keep it to yourself.

## How to get it

**You need:**
- Your own **SSX 3 for the original Xbox, PAL version**: a disc image (`.iso`) or the game's files
- A **Windows 10/11 PC** with about 15 GB free
- A **Switch with custom firmware** and the Homebrew Menu, with about 7.5 GB free on the SD card

**Then:**
1. Download **Source code (zip)** from the [latest release](https://github.com/sBananz/SSX3NX/releases/latest) and extract it
   to a short folder like `C:\SSX3NX`.
2. Double-click **`SSX3NX Builder.bat`**.
3. Press the four buttons in order:
   **Install / check** → **Disc image...** (pick your `.iso`) → **Build SSX3NX.nro** → **Copy to SD card**

The first time takes a while: installing the tools (one restart) and a 20–40 minute build. After that, it's done.

📖 Stuck? The **[step-by-step guide](docs/windows-guide.md)** explains every click.

## Playing

On the Switch, **hold R while starting any game** to open the Homebrew Menu, then pick **SSX 3 (SSX3NX)**.
(Starting it from the Album doesn't give it enough memory.)

The first start takes a few extra minutes while it sets up its save space (about 5.2 GB) on the SD card.

> [!WARNING]
> Your saves are in the `Partition*.img` files in `switch/ssx3nx/` on the SD card. Don't delete them.
> To update to a new version, just replace `SSX3NX.nro`.

## What's in it

- 🏂 **30 FPS races and 60 FPS menus** on the Switch's normal clocks. The game itself still runs at its original 60 Hz underneath.
- 📺 **Widescreen**, and **1080p when docked** (it switches over even mid-race). A Switch Lite can use the sharper docked picture too.
- 🎮 **Switch controls**: Switch button prompts everywhere, a Pro Controller on the controller pages, and the PS2-style riding layout.
- ⚡ **Boost speed lines**: speed lines and a soft edge blur while you boost, in the spirit of *SSX* (2012) and *SSX On Tour*.
- 💾 **Saves, music and speech** all work.

## Controls

| Riding | |
|---|---|
| **B** | Jump |
| **Y** | Boost / tweak |
| **A** | Handplant |
| **L R ZL ZR** | The four grabs |
| **X** or **−** | Reset |

Menus use **A** to confirm and **B** to go back. Joy-Cons and the Pro Controller both work.

## Settings

All the settings are in **`SSX 3 env.txt`** in `switch/ssx3nx/` on the SD card, each one explained. Open it with Notepad.
Popular ones:

| Setting | What it does |
|---|---|
| `RECOMP_SKIP_INTRO=2` | Skips the 3.5-minute intro movie (`1` skips the logos too). |
| `RECOMP_FPS_OVERLAY=1` | Shows the frame rate in the corner. |
| `SSX3NX_BOOST_LINES=2` | Boost speed lines without the blur (`0` turns them off). |
| `SSX3NX_PS2_CONTROLS=0` | The Xbox button layout instead of the PS2 one. |
| `SSX3NX_DOCKED=1` | On a Switch Lite: the sharper docked picture. |

To turn a setting on, remove the `#` in front of it.

## If something goes wrong

| Problem | Try |
|---|---|
| The builder stops | It shows what went wrong; the full messages are in `build\windows\` in the SSX3NX folder. See the [guide](docs/windows-guide.md#if-something-goes-wrong). |
| "Not enough memory" on the Switch | Start it by holding R over a game, not from the Album. |
| Black screen or broken picture | `RECOMP_VK_PRESENT=0` in the settings file |
| Textures look wrong | `RECOMP_TEX_DXT_NATIVE=0` |
| Objects popping out, flickering textures or wrong colours | `SSX3NX_NATIVE_RENDER=0` |
| Crackling music or speech | `RECOMP_APU_STREAM_CUSHION_MS=120` |

More switches are in section 6 (Troubleshooting) of the settings file.

**Reporting a bug:** set `RECOMP_LOG_LEVEL=info` in the settings file, make it happen again, and
[open an issue](https://github.com/sBananz/SSX3NX/issues) with what you did, what happened, and `SSX3NX.log` from the SD card.

**Known issue:** the few seconds of flyover before a race starts can dip to 27–28 FPS. Races themselves hold 30.

## Linux, macOS, or building by hand

See **[docs/building.md](docs/building.md)**. It also explains how the port works.

## Credits and acknowledgments

SSX3NX stands on the work of these projects and people:

| Who | What |
|---|---|
| [**sp00nznet**](https://github.com/sp00nznet) | [**xboxrecomp**](https://github.com/sp00nznet/xboxrecomp), the static recompiler and Xbox runtime this whole port is built on (MIT). Also [flatout2-sw](https://github.com/sp00nznet/flatout2-sw), whose Switch clock code the clock handling follows (MIT). |
| [**DevCrashDay**](https://github.com/DevCrashDay) | [**OpenTrickyNX**](https://github.com/DevCrashDay/OpenTrickyNX), the SSX Tricky Switch port. Its Switch platform layer (libnx entry, settings file, clocks, audio, input, Mesa NVK renderer work, Switch save fixes), build scripts and Windows builder were adapted for this port (GPL-3.0-only). |
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

# Building SSX3NX by hand

**On Windows, you don't need this page:** the [SSX3NX Builder](windows-guide.md) installs everything and builds the game
for you. This page is for Linux and macOS, or for building without the builder.

## What you need

| | |
|---|---|
| **Docker** | Docker Desktop on Windows and macOS (started before you build), or Docker Engine on Linux. It provides the Switch compiler: devkitPro's `devkitpro/devkita64` image, about 3 GB, downloaded on the first build. An installed devkitPro works too (`DEVKITPRO=...`). |
| **On Windows** | [MSYS2](https://www.msys2.org/). In its **UCRT64** shell: `pacman -S --needed git mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone` |
| **On Linux or macOS** | Git, Python 3 and `capstone` (`scripts/setup.sh` sets up a virtual environment with it). |
| **Vulkan driver files** | [mesa-switch-nvk-sdk.zip](https://github.com/DevCrashDay/OpenTrickyNX/releases/download/nvk-sdk/mesa-switch-nvk-sdk.zip) from OpenTrickyNX. Unzip it and keep its `portlibs/switch` folder. Without it, the build only has the (far too slow) software renderer. |
| **The game** | Your own ***SSX 3* Xbox disc, PAL release**, as files: `default.xbe` and the `data` folder. `python3 tools/extract_iso.py game.iso SSX3` unpacks a disc image. Other regions and the PS2 / GameCube versions won't work. |

## 1. Lay out the folders

The scripts expect three folders side by side:

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

## 2. Set up the toolkit (once)

This clones xboxrecomp v0.13.1 next to the repo and applies `patches/xboxrecomp-local.patch` (SSX 3's runtime changes) and
`patches/xboxrecomp-switch.patch` (the Switch layer):

```bash
bash scripts/setup.sh
```

On Windows, run it from the MSYS2 **UCRT64** shell, in the repository folder.

## 3. Build

Start Docker first.

**Windows** (the batch file opens MSYS2 for you):

```powershell
$env:NVK_SDK = "C:\path\to\portlibs\switch"
.\build_switch.bat
```

**Linux or macOS:**

```bash
NVK_SDK=/path/to/portlibs/switch ./build_switch.sh
```

The script translates your `default.xbe` to C (a few minutes, only the first time) and compiles
`dist/switch/ssx3nx/SSX3NX.nro` (10 to 30 minutes the first time, then only what changed). The output folder also gets
`SSX 3 env.txt` and a player `README.txt`.

| Build option | |
|---|---|
| `XBE=/path/to/default.xbe` | Your game's `default.xbe`, if it isn't in `../SSX3/`. |
| `SWITCH_JOBS=4` | Compile fewer files at once. The translated game code takes a few GB of memory per file: use this if Docker runs short of memory. |
| `SWITCH_OPTS="..."` | Extra CMake options, kept by the build folder. |
| `TK=/path/to/xboxrecomp` | The toolkit, if it isn't next to the repo. |

In PowerShell, set these as `$env:SWITCH_JOBS = "4"`.

## 4. Copy to the SD card

1. Copy `dist/switch/ssx3nx/` to `sdmc:/switch/` on your SD card.
2. Copy your game files into a `game` folder inside it, so that `sdmc:/switch/ssx3nx/game/default.xbe` and
   `sdmc:/switch/ssx3nx/game/data/` exist.

Then play as described in the [README](../README.md#playing).

## How it works

SSX3NX is not an emulator: the game's own Xbox code is translated to C with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and compiled for the Switch's ARM CPU. The Xbox's graphics, sound and
controller hardware are reimplemented on top of Vulkan (Mesa NVK), `audout` and `hid`, through devkitPro's libnx. It is the
Switch port of [SSX3-recomp](https://github.com/koenhendriks/SSX3-recomp), with the Switch platform layer from
[OpenTrickyNX](https://github.com/DevCrashDay/OpenTrickyNX).

The same sources still build the PC version (Windows and Linux): see [PC-README.md](PC-README.md).

#!/usr/bin/env bash
# SSX3NX -- build SSX 3 for the Nintendo Switch from your own PAL default.xbe.
#
#   ./build_switch.sh                    # XBE defaults as in scripts/regen.sh
#   XBE=/c/SSX3/default.xbe ./build_switch.sh
#
# Runs on Linux or Windows (build_switch.bat, through MSYS2) with Docker
# (Docker Desktop on Windows). The Switch toolchain is the ssx3nx-build image:
# devkitpro/devkita64 plus glslangValidator (Dockerfile), built on first use.
# Set DEVKITPRO instead to use an installed devkitPro and skip Docker.
#
# Port of OpenTrickyNX's build_switch.sh (GPL-3.0-only). See PORTING_PLAN.md.
#
#   NVK_SDK      mesa-switch built with NVK (portlibs/switch of the
#                "mesa-switch-nvk-sdk" download). Default: the one the
#                OpenTrickyNX builder already downloaded. Without one the
#                build has the software rasteriser only (RECOMP_GPU=sw).
#   SWITCH_JOBS  files compiled at once (default: every core). The generated
#                files take a few GB of memory each: lower it if Docker runs
#                short of memory, e.g. SWITCH_JOBS=4.
#   SWITCH_OPTS  extra CMake options kept by the build folder,
#                e.g. SWITCH_OPTS="-DSSX3_VULKAN=OFF".
#   SKIP_REGEN=1 do not run scripts/regen.sh even when src/recomp/gen is missing.
#
# Result: dist/switch/ssx3nx/SSX3NX.nro. Copy that folder to sdmc:/switch/ with
# your rip in ssx3nx/game/ (default.xbe + data/). The build contains code
# translated from your copy of the game: keep it to yourself.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ON_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) ON_WINDOWS=1 ;; esac

echo "== [0/4] checking the tools"
SWITCH_CMAKE="${DEVKITPRO:-/nonexistent}/portlibs/switch/bin/aarch64-none-elf-cmake"
NATIVE=0
[ -n "${DEVKITPRO:-}" ] && [ -f "$SWITCH_CMAKE" ] && NATIVE=1
if [ "$NATIVE" = 0 ]; then
    command -v docker >/dev/null 2>&1 || { echo "Needs Docker or an installed devkitPro (DEVKITPRO=...)."; exit 1; }
    docker info >/dev/null 2>&1 || { echo "Docker is not running: start Docker Desktop and try again."; exit 1; }
    if ! docker image inspect ssx3nx-build >/dev/null 2>&1; then
        echo "   building the ssx3nx-build image (once)"
        docker build -t ssx3nx-build - < Dockerfile
    fi
fi
# The toolkit checkout next to this repo (scripts/setup.sh makes it).
TK="${TK:-$(dirname "$ROOT")/xboxrecomp}"
[ -f "$TK/CMakeLists.txt" ] || { echo "No xboxrecomp toolkit at $TK: run scripts/setup.sh first."; exit 1; }
grep -q "XBOX_PAGEMAP" "$TK/src/kernel/guest_ptr.h" ||
    { echo "The toolkit at $TK lacks the Switch changes: git -C \"$TK\" apply patches/xboxrecomp-switch.patch"; exit 1; }

if [ -z "${NVK_SDK:-}" ]; then
    for c in "/c/OpenTrickyNX-main/build/windows/nvk-sdk/portlibs/switch" "$ROOT/build/nvk-sdk/portlibs/switch"; do
        [ -f "$c/lib/libvulkan.a" ] && { NVK_SDK="$c"; break; }
    done
fi
[ "$ON_WINDOWS" = 1 ] && [ -n "${NVK_SDK:-}" ] && NVK_SDK="$(cygpath -u "$NVK_SDK")"
if [ -n "${NVK_SDK:-}" ] && [ -f "$NVK_SDK/lib/libvulkan.a" ]; then
    echo "   NVK SDK: $NVK_SDK (Vulkan renderer on)"
else
    NVK_SDK=""
    echo "   no NVK SDK: software rasteriser only"
fi

echo "== [1/4] toolkit: $TK"

echo "== [2/4] translated game code"
if [ -n "$(ls src/recomp/gen/*.c 2>/dev/null)" ]; then
    echo "   src/recomp/gen present (re-run scripts/regen.sh after changing overrides)"
elif [ "${SKIP_REGEN:-0}" = 1 ]; then
    echo "   src/recomp/gen is empty and SKIP_REGEN=1"; exit 1
else
    PY="${PY:-}"
    if [ -z "$PY" ] && [ "$ON_WINDOWS" = 1 ]; then PY="${MINGW_PREFIX:-/ucrt64}/bin/python.exe"; fi
    PY="${PY:-python3}"
    XBE="${XBE:-}" PY="$PY" TK="$TK" bash scripts/regen.sh
fi

echo "== [3/4] compiling for the Switch"
BUILD=build-switch
CONFIGURE_ARGS="-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG -ffile-prefix-map=/src/='"
if [ "$NATIVE" = 1 ]; then
    [ -f "$BUILD/CMakeCache.txt" ] || eval "\"$SWITCH_CMAKE\" -S . -B $BUILD -DXBOXRECOMP_DIR=\"$TK\" ${NVK_SDK:+-DNVK_SDK=\"$NVK_SDK\"} $CONFIGURE_ARGS"
    [ -z "${SWITCH_OPTS:-}" ] || cmake $SWITCH_OPTS "$BUILD"
    cmake --build "$BUILD" -j "${SWITCH_JOBS:-$(nproc 2>/dev/null || echo 4)}"
else
    # The Docker build needs a folder configured in Docker (at /src).
    if [ -f "$BUILD/CMakeCache.txt" ] && ! grep -qxF "CMAKE_CACHEFILE_DIR:INTERNAL=/src/$BUILD" "$BUILD/CMakeCache.txt"; then
        echo "   $BUILD was configured elsewhere: configuring it again"
        rm -rf "$BUILD"
    fi
    if [ "$ON_WINDOWS" = 1 ]; then
        MOUNT="$(cygpath -m "$ROOT")"; TK_M="$(cygpath -m "$TK")"; USER_ARGS=()
        NVK_M=""; [ -n "$NVK_SDK" ] && NVK_M="$(cygpath -m "$NVK_SDK")"
        export MSYS2_ARG_CONV_EXCL='*'
    else
        MOUNT="$ROOT"; TK_M="$TK"; NVK_M="$NVK_SDK"; USER_ARGS=(-u "$(id -u):$(id -g)")
    fi
    DOCKER_NVK=(); NVK_ARG=""
    if [ -n "$NVK_M" ]; then DOCKER_NVK=(-v "$NVK_M:/opt/nvk:ro"); NVK_ARG="-DNVK_SDK=/opt/nvk"; fi
    docker run --rm "${USER_ARGS[@]}" ${DOCKER_NVK[@]+"${DOCKER_NVK[@]}"} \
        -v "$MOUNT:/src" -v "$TK_M:/xboxrecomp" -w /src ssx3nx-build bash -c "
        set -e
        [ -f $BUILD/CMakeCache.txt ] || /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake \
            -S /src -B $BUILD -G Ninja -DXBOXRECOMP_DIR=/xboxrecomp $NVK_ARG $CONFIGURE_ARGS
        ${SWITCH_OPTS:+cmake $SWITCH_OPTS $BUILD}
        cmake --build $BUILD -j ${SWITCH_JOBS:-\$(nproc)} ${SWITCH_KEEP_GOING:+-- -k 0}"
fi

echo "== [4/4] dist/switch/ssx3nx"
OUT=dist/switch/ssx3nx
mkdir -p "$OUT"
cp "$BUILD/SSX3NX.nro" "$OUT/"
[ ! -f "$BUILD/SSX3NX.sym" ] || cp "$BUILD/SSX3NX.sym" "$OUT/"
# The settings file, unless one is there already (it may hold your changes).
[ -f "$OUT/SSX 3 env.txt" ] || cp "switch/SSX 3 env.txt" "$OUT/"
cp "switch/README.txt" "$OUT/"
echo
echo "Done: $OUT/SSX3NX.nro (settings: $OUT/SSX 3 env.txt)"
echo "Copy $OUT to sdmc:/switch/ on your SD card, put your rip in ssx3nx/game/,"
echo "and start it from the Homebrew Menu opened over a game (hold R)."

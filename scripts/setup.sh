#!/usr/bin/env bash
# One-time setup of the toolkit next to this repo, the way the work was done:
#
#   <parent>/xboxrecomp     sp00nznet/xboxrecomp at XBOXRECOMP_BASE + patches/xboxrecomp-local.patch
#   <parent>/ssx3-recomp    this repo
#   <parent>/SSX3           your own rip: default.xbe + data/ (not in git)
#
# TK=<folder> puts the toolkit elsewhere (the SSX3NX Builder uses build/xboxrecomp).
# Also creates xboxrecomp/.venv with capstone (on Windows, MSYS2's Python with
# pacman's capstone is used instead). Safe to re-run: a toolkit that already has
# the changes is never touched.
set -euo pipefail

XBOXRECOMP_BASE=193e299               # v0.13.1 "Cut Short"
XBOXRECOMP_URL=${XBOXRECOMP_URL:-https://github.com/sp00nznet/xboxrecomp.git}

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PARENT="$(dirname "$HERE")"
TK="${TK:-$PARENT/xboxrecomp}"     # the SSX3NX Builder keeps it in build/xboxrecomp

CLONED=0
if [ ! -d "$TK/.git" ]; then
    git clone "$XBOXRECOMP_URL" "$TK"
    CLONED=1
fi
# Never move an existing toolkit: one that already has the changes (applied,
# or committed on a branch, as in development) is left as it is. Only a clone
# made just now, or a clean checkout of the base commit, is patched.
at_base=0
[ "$(git -C "$TK" rev-parse HEAD)" = "$(git -C "$TK" rev-parse "$XBOXRECOMP_BASE^{commit}")" ] && at_base=1
clean=0
[ -z "$(git -C "$TK" status --porcelain --untracked-files=no)" ] && clean=1
if grep -q "XBOX_PAGEMAP" "$TK/src/kernel/guest_ptr.h" 2>/dev/null; then
    echo "toolkit: already has the SSX 3 and Switch changes"
elif git -C "$TK" apply --reverse --check "$HERE/patches/xboxrecomp-local.patch" 2>/dev/null; then
    git -C "$TK" apply "$HERE/patches/xboxrecomp-switch.patch"
    echo "toolkit: Switch patch applied on top of the local patch"
elif [ "$clean" = 1 ] && { [ "$CLONED" = 1 ] || [ "$at_base" = 1 ]; }; then
    git -C "$TK" checkout -q "$XBOXRECOMP_BASE"
    git -C "$TK" apply "$HERE/patches/xboxrecomp-local.patch"
    # SSX3NX: the Switch layer on top (page-mapped guest memory, NVK, audout...)
    git -C "$TK" apply "$HERE/patches/xboxrecomp-switch.patch"
    echo "toolkit: $XBOXRECOMP_BASE + local and Switch patches applied"
else
    echo "toolkit at $TK has other changes and lacks the SSX 3 / Switch ones; left alone." >&2
    echo "Delete that folder and run this again for a fresh one." >&2
    exit 1
fi

# Windows (MSYS2): the UCRT64 Python with pacman's capstone does the work
# (build_switch.sh passes it to regen.sh as PY), no venv.
case "$(uname -s)" in MINGW*|MSYS*) PY="${PY:-${MINGW_PREFIX:-/ucrt64}/bin/python.exe}" ;; esac
if [ -n "${PY:-}" ]; then
    if "$PY" -c "import capstone" 2>/dev/null; then echo "capstone ok ($PY)"; exit 0; fi
    echo "$PY has no capstone: pacman -S mingw-w64-ucrt-x86_64-python-capstone" >&2
    exit 1
fi

# Python venv with capstone. This WSL box had no pip and no ensurepip, so the
# fallback makes a pip-less venv and unzips the official wheel into it after
# checking its hash.
if ! "$TK/.venv/bin/python" -c "import capstone" 2>/dev/null; then
    if python3 -m venv "$TK/.venv" 2>/dev/null && "$TK/.venv/bin/python" -m pip --version >/dev/null 2>&1; then
        "$TK/.venv/bin/python" -m pip install -q capstone==5.0.9
    else
        rm -rf "$TK/.venv"
        python3 -m venv --without-pip "$TK/.venv"
        WHL=capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl
        SHA=273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e
        TMP="$(mktemp -d)"
        curl -sSL -o "$TMP/$WHL" "https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/$WHL"
        echo "$SHA  $TMP/$WHL" | sha256sum -c -
        SP="$("$TK/.venv/bin/python" -c "import sysconfig; print(sysconfig.get_paths()['purelib'])")"
        python3 -m zipfile -e "$TMP/$WHL" "$SP"
        rm -rf "$TMP"
    fi
fi
"$TK/.venv/bin/python" -c "import capstone; print('capstone ok')"

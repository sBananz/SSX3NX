#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# From OpenTrickyNX (DevCrashDay/OpenTrickyNX, port/tools/extract_iso.py).
"""Copy every file out of your own SSX 3 (PAL) Xbox disc image into a folder.

    extract_iso.py "path/to/SSX 3.iso" game

Reads XDVDFS (the Xbox disc file system) directly: an extract-xiso image or a
full disc dump (the game partition is then found at its usual offset). No
other tool needed. Used by the SSX3NX Builder (tools/windows/builder.ps1).
"""
import os, shutil, struct, sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Where the game partition starts: xiso, then the usual full-dump offsets.
PARTITIONS = (0, 0x18300000, 0x0FD90000, 0x02080000)


def find_partition(f):
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    return None


def read_dir(f, base, sector, size):
    """Directory table -> {name: (sector, size, is_dir)}."""
    if size == 0:
        return {}
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    out = {}
    todo = [0]
    while todo:
        off = todo.pop()
        if off + 14 > len(table):
            continue
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue                                  # padding
        name = table[off + 14:off + 14 + nlen].decode("ascii", "replace")
        out[name] = (start, length, bool(attr & 0x10))
        if left:
            todo.append(left * 4)
        if right:
            todo.append(right * 4)
    return out


def copy_file(f, iso, base, sector, size, dst):
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    f.seek(base + sector * SECTOR)
    left = size
    with open(dst, "wb") as g:
        while left:
            chunk = f.read(min(left, 1 << 20))
            if not chunk:
                sys.exit("%s: truncated image" % iso)
            g.write(chunk)
            left -= len(chunk)


def extract_all(f, iso, base, root, out):
    """Every file on the disc into out/, printing progress by the percent."""
    files = []
    todo = [((), root)]
    while todo:
        parts, (sector, size) = todo.pop()
        for name, (start, length, is_dir) in sorted(read_dir(f, base, sector, size).items()):
            if is_dir:
                todo.append((parts + (name,), (start, length)))
            else:
                files.append((parts + (name,), start, length))
    if not any(len(p) == 1 and p[0].lower() == "default.xbe" for p, _, _ in files):
        sys.exit("%s: default.xbe not found -- is this SSX 3 for Xbox?" % iso)
    total = sum(n for _, _, n in files) or 1
    os.makedirs(out, exist_ok=True)
    free = shutil.disk_usage(out).free
    if free < total + (16 << 20):
        sys.exit("%s: needs %d MB free, there are %d MB" % (out, (total >> 20) + 16, free >> 20))
    done, shown = 0, -1
    for parts, start, length in files:
        copy_file(f, iso, base, start, length, os.path.join(out, *parts))
        done += length
        if done * 100 // total != shown:
            shown = done * 100 // total
            print("   %3d%%  %s" % (shown, "/".join(parts)), flush=True)
    print("   %d files, %d MB" % (len(files), total >> 20))


def main():
    args = [a for a in sys.argv[1:] if a != "--all"]
    if len(args) != 2:
        sys.exit(__doc__)
    iso, out = args
    with open(iso, "rb") as f:
        base = find_partition(f)
        if base is None:
            sys.exit("%s: not an Xbox disc image (no XDVDFS header found)" % iso)
        f.seek(base + 32 * SECTOR + len(MAGIC))
        root = struct.unpack("<II", f.read(8))
        extract_all(f, iso, base, root, out)


if __name__ == "__main__":
    main()

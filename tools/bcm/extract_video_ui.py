#!/usr/bin/env python3
"""
extract_video_ui.py — Extract Apple's overlay bitmaps from bcm_video_ui.bin.

The file uses Apple's BMap/Bits/VUI1 container format.  Each bitmap record
starts with a 4-byte tag followed by its size and pixel data.

Usage:
    python3 extract_video_ui.py <path/to/bcm_video_ui.bin> [--out <dir>]

Copyright (C) 2024 bcm-rockbox contributors
SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import os
import struct
import sys


def _read32be(data, off):
    return struct.unpack_from(">I", data, off)[0]


def extract_vui1(data: bytes, out_dir: str):
    if len(data) < 8:
        print("ERROR: file too short")
        sys.exit(1)

    magic = data[0:4]
    if magic not in (b"VUI1", b"BMap", b"Bits"):
        print(f"WARNING: unexpected magic {magic!r}, trying anyway")

    pos = 0
    idx = 0
    os.makedirs(out_dir, exist_ok=True)

    while pos + 8 <= len(data):
        tag  = data[pos:pos+4]
        size = _read32be(data, pos + 4)
        if size == 0 or pos + 8 + size > len(data):
            break

        payload = data[pos + 8 : pos + 8 + size]
        out_path = os.path.join(out_dir, f"ui_{idx:03d}_{tag.decode('ascii', errors='replace')}.bin")
        with open(out_path, "wb") as f:
            f.write(payload)
        print(f"  Record {idx}: tag={tag!r} size={size} → {os.path.basename(out_path)}")
        idx += 1
        pos += 8 + size
        # Word-align
        if size & 3:
            pos += 4 - (size & 3)

    print(f"Done. {idx} record(s) extracted.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                   formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bin_file", help="Path to bcm_video_ui.bin")
    parser.add_argument("--out", default="ui_bitmaps",
                        help="Output directory (default: ui_bitmaps)")
    args = parser.parse_args()

    if not os.path.exists(args.bin_file):
        print(f"ERROR: {args.bin_file} not found")
        sys.exit(1)

    with open(args.bin_file, "rb") as f:
        data = f.read()

    extract_vui1(data, args.out)


if __name__ == "__main__":
    main()

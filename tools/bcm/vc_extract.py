#!/usr/bin/env python3
"""
vc_extract.py — Extract VideoCore firmware files from an iPod .ipsw.

The .ipsw is a ZIP archive.  Inside it there is a Firmware-*.zip (or the
firmware image directly).  The firmware image contains a FAT12 volume at
a known offset; the volume holds the VideoCore files.

The FAT12 volume is identified by:
  • 0x55AA signature at byte 510 of the boot sector
  • OEM string starting with "MTOOL" at byte 3

Usage:
    python3 vc_extract.py <path/to/iPod_25.1.3_1.ipsw> [--out <dir>]

Output: flat directory containing vmcs.bin, mplayer.vll, h264dec.vll, etc.
        (since v56 all VideoCore files are in a flat directory)

Copyright (C) 2024 bcm-rockbox contributors
SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import os
import struct
import sys
import zipfile
import io


# ---------------------------------------------------------------------------
# FAT12 helpers
# ---------------------------------------------------------------------------

SECTOR_SIZE = 512

def _read16(data, off):
    return struct.unpack_from("<H", data, off)[0]

def _read32(data, off):
    return struct.unpack_from("<I", data, off)[0]


class Fat12:
    """Minimal FAT12 reader that understands long-name directory entries."""

    def __init__(self, data: bytes):
        bs = data[:512]
        # Bytes per sector
        self.bps  = _read16(bs, 0x0B)
        # Sectors per cluster
        self.spc  = bs[0x0D]
        # Reserved sectors
        self.rsvd = _read16(bs, 0x0E)
        # Number of FATs
        self.nfat = bs[0x10]
        # Root entry count
        self.nroot = _read16(bs, 0x11)
        # FAT size in sectors
        self.fat_sz = _read16(bs, 0x16)

        self.data = data
        self.fat_off  = self.rsvd * self.bps
        self.root_off = self.fat_off + self.nfat * self.fat_sz * self.bps
        self.data_off = self.root_off + self.nroot * 32

    def _fat_entry(self, cluster: int) -> int:
        offset = self.fat_off + cluster * 3 // 2
        w = _read16(self.data, offset)
        if cluster & 1:
            return (w >> 4) & 0xFFF
        else:
            return w & 0xFFF

    def _cluster_to_offset(self, cluster: int) -> int:
        return self.data_off + (cluster - 2) * self.spc * self.bps

    def _read_chain(self, cluster: int) -> bytes:
        out = bytearray()
        while cluster < 0xFF8:
            off = self._cluster_to_offset(cluster)
            out += self.data[off:off + self.spc * self.bps]
            cluster = self._fat_entry(cluster)
        return bytes(out)

    def _parse_dir(self, data: bytes):
        """
        Yield (long_name_or_short_name, start_cluster, size) tuples.
        Handles LFN entries.
        """
        i = 0
        lfn_parts = {}
        while i + 32 <= len(data):
            entry = data[i:i+32]
            i += 32
            if entry[0] == 0x00:
                break
            if entry[0] == 0xE5:
                lfn_parts.clear()
                continue
            attr = entry[11]
            if attr == 0x0F:
                # LFN entry
                seq = entry[0] & 0x1F
                chars = (entry[1:11] + entry[14:26] + entry[28:32])
                name_part = chars.decode("utf-16-le", errors="replace").rstrip("\x00\xff")
                lfn_parts[seq] = name_part
                continue
            if attr & 0x08:
                lfn_parts.clear()
                continue
            is_dir = bool(attr & 0x10)
            name_raw = entry[0:8].decode("ascii", errors="replace").rstrip()
            ext_raw  = entry[8:11].decode("ascii", errors="replace").rstrip()
            if lfn_parts:
                parts = sorted(lfn_parts.keys())
                long_name = "".join(lfn_parts[k] for k in parts)
            else:
                long_name = (name_raw + ("." + ext_raw if ext_raw else ""))
            lfn_parts.clear()
            cluster_hi = _read16(entry, 20)
            cluster_lo = _read16(entry, 26)
            cluster = (cluster_hi << 16) | cluster_lo
            size = _read32(entry, 28)
            yield long_name, cluster, size, is_dir

    def list_root(self):
        root_data = self.data[self.root_off:self.root_off + self.nroot * 32]
        return list(self._parse_dir(root_data))

    def list_dir(self, cluster: int):
        dir_data = self._read_chain(cluster)
        return list(self._parse_dir(dir_data))

    def read_file(self, cluster: int, size: int) -> bytes:
        raw = self._read_chain(cluster)
        return raw[:size]

    def walk(self, cluster=0, path=""):
        """
        Yield (path, cluster, size) for every file (recursively).
        cluster=0 means the root directory.
        """
        entries = self.list_root() if cluster == 0 else self.list_dir(cluster)
        for name, cl, size, is_dir in entries:
            if name in (".", ".."):
                continue
            full = path + "/" + name if path else name
            if is_dir:
                yield from self.walk(cl, full)
            else:
                yield full, cl, size


# ---------------------------------------------------------------------------
# IPSW / firmware extraction
# ---------------------------------------------------------------------------

def find_fat12(data: bytes) -> int:
    """Return the byte offset of the FAT12 volume, or -1 if not found."""
    pos = 0
    while pos + 512 <= len(data):
        # Look for 0x55AA boot signature
        if data[pos + 510] == 0x55 and data[pos + 511] == 0xAA:
            oem = data[pos + 3:pos + 11]
            if oem[:5] == b"MTOOL":
                return pos
        pos += 512
    return -1


def extract_ipsw(ipsw_path: str, out_dir: str):
    print(f"Opening {ipsw_path} ...")
    with zipfile.ZipFile(ipsw_path, "r") as z:
        names = z.namelist()
        # Find the firmware payload (Firmware-*.zip or WTF*.ipsw or similar)
        fw_name = None
        for n in names:
            bn = os.path.basename(n).lower()
            if bn.startswith("firmware") and bn.endswith(".zip"):
                fw_name = n
                break
        if not fw_name:
            # Try direct .dmg or raw firmware
            for n in names:
                if "firmware" in n.lower() and not n.endswith("/"):
                    fw_name = n
                    break

        if not fw_name:
            print("ERROR: could not find firmware payload inside .ipsw")
            sys.exit(1)

        print(f"  Found firmware payload: {fw_name}")
        fw_data = z.read(fw_name)

    # If the payload itself is a ZIP, open it
    if fw_data[:2] == b"PK":
        print("  Firmware payload is a ZIP, opening inner archive ...")
        inner = zipfile.ZipFile(io.BytesIO(fw_data))
        # Find the raw binary
        for n in inner.namelist():
            bn = os.path.basename(n).lower()
            if not n.endswith("/") and bn not in ("info.plist", "manifest.plist"):
                fw_data = inner.read(n)
                print(f"  Using inner file: {n}")
                break
        inner.close()

    # Find FAT12 volume
    fat_off = find_fat12(fw_data)
    if fat_off < 0:
        print("ERROR: FAT12 volume (MTOOL OEM, 0x55AA) not found in firmware")
        sys.exit(1)

    print(f"  FAT12 volume at offset 0x{fat_off:08X}")
    fat_data = fw_data[fat_off:]
    fs = Fat12(fat_data)

    # Walk and extract all files
    os.makedirs(out_dir, exist_ok=True)
    n_extracted = 0
    for path, cl, size in fs.walk():
        # Flatten: drop directory components (since v56 layout is flat)
        basename = os.path.basename(path.replace("\\", "/"))
        out_path = os.path.join(out_dir, basename)
        content = fs.read_file(cl, size)
        with open(out_path, "wb") as f:
            f.write(content)
        print(f"  Extracted: {path} → {basename} ({size} bytes)")
        n_extracted += 1

    print(f"Done. {n_extracted} file(s) extracted to {out_dir}/")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                   formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ipsw", help="Path to .ipsw file")
    parser.add_argument("--out", default="Resources/VideoCore",
                        help="Output directory (default: Resources/VideoCore)")
    args = parser.parse_args()

    if not os.path.exists(args.ipsw):
        print(f"ERROR: {args.ipsw} not found")
        sys.exit(1)

    extract_ipsw(args.ipsw, args.out)


if __name__ == "__main__":
    main()

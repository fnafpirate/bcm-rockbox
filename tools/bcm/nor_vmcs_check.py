#!/usr/bin/env python3
"""
nor_vmcs_check.py — Compare the NOR flash vmcs section to vmcs.bin.

The iPod 5G NOR flash image contains the BCM VideoCore OS (vmcs) at a
fixed offset.  This tool reads a NOR dump and the extracted vmcs.bin and
reports whether they match.

Usage:
    python3 nor_vmcs_check.py <nor_dump.bin> <vmcs.bin>

Copyright (C) 2024 bcm-rockbox contributors
SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import hashlib
import sys


VMCS_OFFSET = 0x0  # scan from start since exact offset varies by firmware version
SCAN_STEP   = 0x1000


def find_vmcs_in_nor(nor_data: bytes, vmcs_data: bytes) -> int:
    """Return the offset of vmcs_data inside nor_data, or -1."""
    needle = vmcs_data[:64]  # first 64 bytes as fingerprint
    off = 0
    while off + len(vmcs_data) <= len(nor_data):
        if nor_data[off:off+64] == needle:
            if nor_data[off:off+len(vmcs_data)] == vmcs_data:
                return off
        off += SCAN_STEP
    return -1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                   formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("nor_dump", help="NOR flash dump binary")
    parser.add_argument("vmcs_bin", help="Extracted vmcs.bin")
    args = parser.parse_args()

    with open(args.nor_dump, "rb") as f:
        nor_data = f.read()
    with open(args.vmcs_bin, "rb") as f:
        vmcs_data = f.read()

    print(f"NOR size:  {len(nor_data):,} bytes")
    print(f"vmcs size: {len(vmcs_data):,} bytes")
    print(f"vmcs MD5:  {hashlib.md5(vmcs_data).hexdigest()}")

    off = find_vmcs_in_nor(nor_data, vmcs_data)
    if off >= 0:
        print(f"MATCH at NOR offset 0x{off:08X}")
        sys.exit(0)
    else:
        print("NO MATCH found — NOR flash does not contain this vmcs.bin")
        # Show what is at typical offsets
        for probe in [0x60000, 0x80000, 0xA0000, 0xC0000]:
            if probe + 8 <= len(nor_data):
                snippet = nor_data[probe:probe+8].hex()
                print(f"  NOR[0x{probe:06X}]: {snippet}")
        sys.exit(1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
vc_elf_callgraph.py — Print the import call graph of a VideoCore .vll ELF.

The .vll files are ELF shared libraries for VideoCore II.  This tool reads
the dynamic symbol table and prints the imported (undefined) symbols to show
which functions each library calls from the VideoCore OS or other .vll files.

Usage:
    python3 vc_elf_callgraph.py <library.vll>

Copyright (C) 2024 bcm-rockbox contributors
SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import struct
import sys


def _u16(data, off): return struct.unpack_from("<H", data, off)[0]
def _u32(data, off): return struct.unpack_from("<I", data, off)[0]


def parse_elf_imports(data: bytes):
    if data[:4] != b"\x7fELF":
        raise ValueError("Not an ELF file")

    e_phoff   = _u32(data, 0x1C)
    e_phentsize = _u16(data, 0x2A)
    e_phnum   = _u16(data, 0x2C)

    # Find PT_DYNAMIC segment
    dyn_off = dyn_size = 0
    for i in range(e_phnum):
        base = e_phoff + i * e_phentsize
        p_type   = _u32(data, base)
        p_offset = _u32(data, base + 4)
        p_filesz = _u32(data, base + 16)
        if p_type == 2:  # PT_DYNAMIC
            dyn_off  = p_offset
            dyn_size = p_filesz
            break

    if not dyn_off:
        return []

    # Walk dynamic section to find DT_SYMTAB and DT_STRTAB
    symtab_off = strtab_off = strtab_size = 0
    pos = dyn_off
    while pos + 8 <= dyn_off + dyn_size:
        d_tag = _u32(data, pos)
        d_val = _u32(data, pos + 4)
        if   d_tag == 6:  symtab_off  = d_val
        elif d_tag == 5:  strtab_off  = d_val
        elif d_tag == 10: strtab_size = d_val
        elif d_tag == 0:  break
        pos += 8

    if not symtab_off or not strtab_off:
        return []

    # Collect undefined symbols (st_shndx == 0 == SHN_UNDEF)
    imports = []
    sym_pos = symtab_off
    sym_size = 16  # Elf32_Sym
    while sym_pos + sym_size <= len(data):
        st_name  = _u32(data, sym_pos)
        st_shndx = _u16(data, sym_pos + 14)
        if st_shndx == 0 and st_name != 0:
            name_start = strtab_off + st_name
            name_end   = data.index(b"\x00", name_start)
            imports.append(data[name_start:name_end].decode("ascii", errors="replace"))
        sym_pos += sym_size
        # Heuristic: stop after reasonable count
        if len(imports) > 2000:
            break

    return imports


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                   formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("vll", help="Path to .vll ELF file")
    args = parser.parse_args()

    with open(args.vll, "rb") as f:
        data = f.read()

    try:
        imports = parse_elf_imports(data)
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(1)

    if not imports:
        print("No imported symbols found.")
    else:
        print(f"{len(imports)} imported symbol(s):")
        for sym in sorted(set(imports)):
            print(f"  {sym}")


if __name__ == "__main__":
    main()

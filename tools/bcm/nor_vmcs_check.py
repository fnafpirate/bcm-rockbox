#!/usr/bin/env python3
"""
nor_vmcs_check.py - compare the VideoCore OS ('vmcs') section of an iPod Video NOR dump with vmcs.bin.

Get the dump on the device: Rockbox -> Debug -> "Dump ROM contents" writes
/internal_rom_000000-0FFFFF.bin (1 MiB, read from 0x20000000) to the FAT32 volume.

    python nor_vmcs_check.py internal_rom_000000-0FFFFF.bin vmcs.bin [--list] [--extract out.bin]

Directory format (from Rockbox firmware/target/arm/ipod/video/lcd-video.c, flash_get_section()):
u32 words at 0xffe00, 10 words per entry: [0]='flsh', [1]=section id (e.g. 'vmcs'),
[3]=offset, [4]=length, [7]=byte-sum checksum. The ROM_ID() constants are built big-endian in
C but stored as native little-endian words, so the bytes in the dump read 'hslf' / 'scmv'.
"""
import struct, sys, hashlib

def parse(rom):
    ents, p = [], 0xffe00
    while p + 40 <= len(rom):
        w = struct.unpack_from('<10I', rom, p)
        if w[0] != int.from_bytes(b'flsh', 'big'):
            break
        ents.append((w[1].to_bytes(4, 'big').decode('latin1'), w[3], w[4], w[7]))
        p += 40
    return ents

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    if len(a) < 1:
        sys.exit(__doc__)
    rom = open(a[0], 'rb').read()
    ents = parse(rom)
    print(f'{a[0]}: {len(rom)} bytes, {len(ents)} directory entries')
    for name, off, ln, ck in ents:
        s = rom[off:off + ln]
        ok = (sum(s) & 0xffffffff) == ck
        print(f'  {name!r:8} offset={off:#08x} length={ln:8d} checksum {"OK " if ok else "BAD"} sha1={hashlib.sha1(s).hexdigest()[:12]}')
    v = [e for e in ents if e[0] == 'vmcs']
    if not v:
        sys.exit('no vmcs section found')
    _, off, ln, _ = v[0]
    nor = rom[off:off + ln]
    if '--extract' in sys.argv:
        open(sys.argv[sys.argv.index('--extract') + 1], 'wb').write(nor)
    if len(a) >= 2:
        ref = open(a[1], 'rb').read()
        print(f'NOR vmcs: {len(nor)} bytes, sum={sum(nor) & 0xffffffff:#x}; file {a[1]}: {len(ref)} bytes, sum={sum(ref) & 0xffffffff:#x}')
        if nor == ref:
            print('IDENTICAL: the NOR vmcs is byte-for-byte the vmcs.bin from the firmware package.')
        else:
            n = min(len(nor), len(ref))
            first = next((i for i in range(n) if nor[i] != ref[i]), None)
            print('DIFFERENT' + (f', first difference at offset {first:#x}' if first is not None else ', length differs only'))

if __name__ == '__main__':
    main()

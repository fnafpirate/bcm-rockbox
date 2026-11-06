#!/usr/bin/env python3
"""extract_video_ui.py - cut the stock video-playback overlay bitmaps out of YOUR iPod's Apple firmware.

    python3 extract_video_ui.py <firmware file> [-o bcm_video_ui.bin] [--png DIR]

<firmware file> = anything that contains the Apple OS image: the firmware partition dump, an extracted osos image,
or the Firmware-*.ipsw / the firmware file inside it (the UI resource block is found by its signature).
Copy the output to /.rockbox/bcm_video_ui.bin on the iPod.  The bitmaps are Apple's, so they are not part of the
Rockbox source; the BCM test reads them at run time.

UI resource block (iPod Video 1.3, osos offset 0x3325a0): u32 3, u32 header length, u32 8 sections, then 8 x
{tag (reversed fourcc), count, flag, table offset}: BMap Bits LDTm Menu Str Type View TCmd.  BMap entries are 12 bytes
{s16 id, s16 ?, u32 offset (from the end of the header), u32 size}; each bitmap starts with a 28-byte header
{u16 format, u16 ?, u16 bytes per row, u16 depth, 8 bytes, u32 height, u32 width, u32 data length}.  Format 0x1444 is
ARGB4444 (alpha in the top nibble), 0x565 is RGB565.

Output (VUI1): "VUI1", u32 count, count x {u16 key, u16 w, u16 h, u16 format, u32 offset}, then packed pixel rows.
"""
import struct, sys, argparse, os

# key -> stock resource id (video overlay views 22056..22065, see the findings)
KEYS = {0: 21922, 1: 21923, 2: 21931, 3: 21932, 4: 21933, 5: 21934, 20: 21949, 21: 21950, 22: 21927}   # 22 = hold lock (v50)
KEYS.update({10 + i: 21939 + i for i in range(10)})
SIG = struct.pack('<I', 3)

def find_block(d):
    o = 0
    while True:
        o = d.find(b'paMB', o)
        if o < 0: return None
        h = o - 12
        if h >= 0 and d[h:h + 4] == SIG and struct.unpack_from('<I', d, h + 8)[0] == 8 and d[o + 16:o + 20] == b'stiB':
            return h
        o += 1

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('firmware'); ap.add_argument('-o', '--out', default='bcm_video_ui.bin'); ap.add_argument('--png')
    a = ap.parse_args()
    d = open(a.firmware, 'rb').read()
    off = find_block(d)
    if off is None and d[:2] == b'PK':                    # an .ipsw is a zip: search its members
        import zipfile, io
        for n in zipfile.ZipFile(io.BytesIO(d)).namelist():
            m = zipfile.ZipFile(io.BytesIO(d)).read(n)
            if find_block(m) is not None: d = m; off = find_block(d); break
    if off is None: sys.exit('UI resource block not found (is this an iPod Video firmware?)')
    z1, length, ns = struct.unpack_from('<III', d, off)
    secs = {}
    for i in range(ns):
        tag = d[off + 12 + 16 * i:off + 16 + 16 * i][::-1].decode('latin1')
        secs[tag] = struct.unpack_from('<III', d, off + 16 + 16 * i)
    cnt, _, to = secs['BMap']
    bmps = {}
    for i in range(cnt):
        rid, _, ro, rs = struct.unpack_from('<hhII', d, off + to + 12 * i)
        p = d[off + length + ro: off + length + ro + rs]
        fmt, _, bpr, depth, _, h, w, blen = struct.unpack_from('<HHHH8sIII', p, 0)
        bmps[rid] = (fmt, bpr, w, h, p[28:28 + blen])
    print('resource block at 0x%x: %d bitmaps' % (off, cnt))
    ents, data = [], b''
    hdr_len = 8 + 12 * len(KEYS)
    for key, rid in sorted(KEYS.items()):
        if rid not in bmps: sys.exit('bitmap %d missing -- different firmware version?' % rid)
        fmt, bpr, w, h, px = bmps[rid]
        if fmt != 0x1444: sys.exit('bitmap %d has format 0x%x, expected 0x1444' % (rid, fmt))
        rows = b''.join(px[y * bpr:y * bpr + 2 * w] for y in range(h))
        ents.append(struct.pack('<HHHHI', key, w, h, fmt, hdr_len + len(data)))
        data += rows
        print('  key %2d <- id %d  %3dx%-3d' % (key, rid, w, h))
        if a.png:
            from PIL import Image
            os.makedirs(a.png, exist_ok=True)
            im = Image.new('RGBA', (w, h))
            for y in range(h):
                for x in range(w):
                    v = struct.unpack_from('<H', rows, 2 * (y * w + x))[0]
                    im.putpixel((x, y), (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 15) * 17))
            im.save(os.path.join(a.png, 'key%02d_id%d.png' % (key, rid)))
    out = b'VUI1' + struct.pack('<I', len(ents)) + b''.join(ents) + data
    open(a.out, 'wb').write(out)
    print('wrote %s (%d bytes) -> copy it to /.rockbox/bcm_video_ui.bin on the iPod' % (a.out, len(out)))

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
vc_extract.py - pull the Resources FAT12 volume out of an iPod 5G/5.5G firmware
image (Firmware-XX.Y.Z, or the .ipsw that contains it) with LONG FILE NAMES intact.

Why LFN matters: the VideoCore libraries are stored as 8.3 short names
(PASSTH~1.VLL, SLIDES~1.VLL, RENDER~1.BIN). A short-name-only parser makes
passthruhandler.vll / slideshow.vll / RenderServer.bin look like they don't exist.

Stdlib only. Read-only on the input; writes only under --out.

    python vc_extract.py iPod_25_1_3.ipsw --out vc_out
    python vc_extract.py Firmware-25.6.3 --list
    python vc_extract.py Firmware-25.6.3 --out vc_out --all      # fonts too (~4 MB)
"""
import argparse
import hashlib
import os
import struct
import sys
import zipfile


# ----------------------------------------------------------------- FAT12 ----
class Fat12:
    def __init__(self, img, off):
        self.img, self.off = img, off
        b = img[off:off + 512]
        self.bps = struct.unpack_from("<H", b, 11)[0]
        self.spc = b[13]
        self.rsv = struct.unpack_from("<H", b, 14)[0]
        self.nfat = b[16]
        self.nroot = struct.unpack_from("<H", b, 17)[0]
        self.tot = struct.unpack_from("<H", b, 19)[0]
        self.spf = struct.unpack_from("<H", b, 22)[0]
        self.oem = b[3:11].decode("latin1")
        fat0 = off + self.rsv * self.bps
        self.fat = img[fat0:fat0 + self.spf * self.bps]
        self.root = off + (self.rsv + self.nfat * self.spf) * self.bps
        self.data = self.root + self.nroot * 32
        self.csize = self.spc * self.bps

    def _next(self, c):
        o = c * 3 // 2
        v = self.fat[o] | (self.fat[o + 1] << 8)
        return (v >> 4) if (c & 1) else (v & 0xFFF)

    def chain(self, c):
        out, seen = [], set()
        while 2 <= c < 0xFF8 and c not in seen:
            seen.add(c)
            out.append(c)
            c = self._next(c)
        return out

    def cluster_off(self, c):
        return self.data + (c - 2) * self.csize

    def read(self, c, size=None):
        buf = b"".join(self.img[self.cluster_off(k):self.cluster_off(k) + self.csize]
                       for k in self.chain(c))
        return buf if size is None else buf[:size]

    @staticmethod
    def _parse_dir(data):
        ents, lfn = [], []
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:                       # LFN fragment
                part = e[1:11] + e[14:26] + e[28:32]
                s = part.decode("utf-16le")
                s = s.split("\x00")[0].split("\uffff")[0]
                lfn.insert(0, s)
                continue
            base = e[:8].decode("latin1").rstrip()
            ext = e[8:11].decode("latin1").rstrip()
            short = base + ("." + ext if ext else "")
            ents.append({
                "name": "".join(lfn) or short,
                "short": short,
                "dir": bool(e[11] & 0x10),
                "cluster": struct.unpack_from("<H", e, 26)[0],
                "size": struct.unpack_from("<I", e, 28)[0],
            })
            lfn = []
        return ents

    def root_entries(self):
        return self._parse_dir(self.img[self.root:self.root + self.nroot * 32])

    def dir_entries(self, cluster):
        return self._parse_dir(self.read(cluster))

    def walk(self, entries=None, prefix=""):
        for e in (self.root_entries() if entries is None else entries):
            if e["name"] in (".", ".."):
                continue
            path = prefix + "/" + e["name"]
            if e["dir"]:
                yield path, e
                yield from self.walk(self.dir_entries(e["cluster"]), path)
            else:
                yield path, e


def find_volume(img):
    """Locate the FAT12 volume by its boot sector instead of hard-coding an offset."""
    for off in range(0, len(img) - 512, 512):
        b = img[off:off + 512]
        if b[510:512] != b"\x55\xAA" or b[3:8] != b"MTOOL":
            continue
        bps = struct.unpack_from("<H", b, 11)[0]
        if bps == 512 and b[13] in (1, 2, 4, 8) and b[16] == 2:
            return off
    return None


def load_image(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            cands = [n for n in z.namelist() if os.path.basename(n).startswith("Firmware-")]
            if not cands:
                sys.exit("no Firmware-* member in " + path)
            return z.read(cands[0]), cands[0]
    with open(path, "rb") as f:
        return f.read(), os.path.basename(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="Firmware-XX.Y.Z or .ipsw")
    ap.add_argument("--out", help="output directory")
    ap.add_argument("--list", action="store_true", help="list only, extract nothing")
    ap.add_argument("--all", action="store_true", help="extract everything (default: VideoCore tree only)")
    a = ap.parse_args()

    img, member = load_image(a.image)
    off = find_volume(img)
    if off is None:
        sys.exit("no MTOOL FAT12 volume found - is this a 5G/5.5G Firmware image?")
    fs = Fat12(img, off)
    print(f"image: {member}  ({len(img)} bytes)")
    print(f"FAT12 volume @ {off:#x}  OEM={fs.oem!r}  {fs.tot} sectors x {fs.bps} B  cluster={fs.csize} B")
    print(f"{'path':58s} {'size':>9s} {'file offset':>12s}  sha1")

    for path, e in fs.walk():
        if e["dir"]:
            continue
        if not a.all and "/videocore/" not in path.lower():
            continue
        data = fs.read(e["cluster"], e["size"]) if e["size"] else b""
        fo = fs.cluster_off(e["cluster"]) if e["cluster"] >= 2 else 0
        print(f"{path:58s} {e['size']:9d} {fo:#12x}  {hashlib.sha1(data).hexdigest()[:12]}"
              + (f"   (short: {e['short']})" if e["short"] != e["name"] else ""))
        if a.out and not a.list:
            dest = os.path.join(a.out, *path.strip("/").split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(data)

    if a.out and not a.list:
        print("\nextracted under", os.path.abspath(a.out))


if __name__ == "__main__":
    main()

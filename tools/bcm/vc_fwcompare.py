#!/usr/bin/env python3
"""vc_fwcompare.py -- identify and compare the VideoCore firmware of iPod firmware releases.  Standard-library Python only.

A "set" is a folder holding what vc_extract.py takes out of an Apple firmware: vmcs.bin, the .vll libraries (mplayer,
h264dec, aacdec, mpg4dec, passthruhandler, slideshow, ...) and anything else (RenderServer.bin).  Sub-folders are searched,
so the extracted Resources/VideoCore tree and the flat /.rockbox/VideoCore folder both work.

  vc_fwcompare.py id      SET                         what each file is: SHA-256, size, kind, build stamp, functions
  vc_fwcompare.py diff    SET_A SET_B [-o REPORT]     file by file: sections, symbols, relocations, changed functions
  vc_fwcompare.py check   SET [--manifest M]          against the combinations tested with bcm-rockbox on hardware
  vc_fwcompare.py manifest SET NAME > M.json          write a manifest for a set you have tested yourself

Changed functions: every function of a library present in both sets is compared byte for byte with the bytes covered by
relocations blanked out, so a function that only moved, or whose callees moved, counts as "same code"; one that differs
outside the relocated fields is "code changed".  For vmcs.bin (no symbols) the gencmd command table strings, the build
stamp and a per-4-KB-block hash map are compared instead.

The tested combination built in here is Firmware-25.6.3 (iPod_25_1_3.ipsw), the set every bcm-rockbox version so far
was tested with.  The host side (bcm_host.c) depends on: the host-interface layout behind VC[0x1FC], the gencmd strings
it sends (mp_*, display_control, power_control, ...), the PDS message layout of passthruhandler/mplayer and the graphics
channel ops 1-4, 8, 9, 0x10 -- a set whose vmcs.bin keeps those strings and whose mplayer.vll / passthruhandler.vll keep
their exported symbols is a candidate; only the hardware can confirm it.
"""
import sys, os, hashlib, struct, json, re, collections

TESTED = {
    "name": "Firmware-25.6.3 (iPod_25_1_3.ipsw), tested with bcm-rockbox v1-v96",
    "files": {
        "RenderServer.bin": "a75c4c2060e6e9d4e85e31ac7ab6f15ce123805e06874c09fc3de3aea2e3e001",
        "aacdec.vll": "0a8c63080120a8af36a2d68ed4ade7eae81eee15f1e344f66cd49a508162c2cc",
        "h264dec.vll": "3915ec9295dc0d9cf9405810a66788e5b544c819347e05a350df6857120a9e8f",
        "mpg4dec.vll": "08ba2f33f267803746c3b3de025e615f4a4b899611d7a4e3eea819f39d3697fe",
        "mplayer.vll": "c100e0cb0782277f81d2b9bceb29340df70d2993e34f77248a8e3c1979bc1fc6",
        "passthruhandler.vll": "0b657b1a4b50198da3ad3d55ecf9d239ffbae8d7da7b3aaca138dcdae5b6d019",
        "slideshow.vll": "3d620d9c6b9bc6884a4cc597e5fad254c09d4f7e5c6275cba4faf2adceea303e",
        "vmcs.bin": "cde4b5a9c9a020b6e23de1be0492d9a853b7d974c4060462d5f57a80e47f55be",
    },
    "sizes": {"vmcs.bin": 201376, "mplayer.vll": 51620, "h264dec.vll": 106960, "aacdec.vll": 52664,
              "mpg4dec.vll": 147232, "passthruhandler.vll": 6528, "slideshow.vll": 47940, "RenderServer.bin": 104540},
}
HOST_GENCMDS = ["version", "set_vll_dir", "load_application", "mp_selectplay", "mp_play", "mp_pause", "mp_stop", "mp_seek",
                "mp_get_status", "mp_get_stats", "mp_setvol", "mp_region", "display_control", "power_control",
                "pm_set_policy", "audio_enable", "tasks", "commands"]
HOST_SYMBOLS = {"passthruhandler.vll": ["passthru_message_handler", "aes_decipher", "aes_init"],
                "mplayer.vll": ["clock_fetch", "codec_name"]}

def sha(d): return hashlib.sha256(d).hexdigest()

def set_gencmds(files):
    """the gencmd names bcm-rockbox sends that appear in the set (vmcs.bin registers some, mplayer.vll the mp_* ones)"""
    blob = b''.join(open(files[n], 'rb').read() for n in ('vmcs.bin', 'mplayer.vll', 'passthruhandler.vll') if n in files)
    return [g for g in HOST_GENCMDS if g.encode() in blob]

def load_set(root):
    files = {}
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if f.endswith(('.vll', '.bin')):
                files.setdefault(f, os.path.join(dp, f))
    return files

class Elf:
    def __init__(self, d):
        self.d = d
        shoff = struct.unpack('<I', d[0x20:0x24])[0]
        shnum, shstr = struct.unpack('<HH', d[0x30:0x34])
        sh = [struct.unpack('<10I', d[shoff + i * 40:shoff + i * 40 + 40]) for i in range(shnum)]
        so = sh[shstr][4]
        def nm(o):
            e = d.find(b'\0', so + o)
            return d[so + o:e].decode('latin-1')
        self.secs = [(nm(x[0]), x) for x in sh]
        sec = dict(self.secs)
        self.syms = []
        ds, st = sec.get('.dynsym'), sec.get('.dynstr')
        if ds and st:
            for i in range(ds[5] // 16):
                n, v, sz, info, oth, shn = struct.unpack('<IIIBBH', d[ds[4] + i * 16:ds[4] + i * 16 + 16])
                e = d.find(b'\0', st[4] + n)
                self.syms.append((d[st[4] + n:e].decode('latin-1'), v, sz, info & 0xf, shn))
        self.rel = []
        for name, x in self.secs:
            if name.startswith('.rela') and x[1] == 4:
                for i in range(x[5] // 12):
                    off, info, add = struct.unpack('<IIi', d[x[4] + i * 12:x[4] + i * 12 + 12])
                    sn = self.syms[info >> 8][0] if (info >> 8) < len(self.syms) else '?'
                    self.rel.append((name, off, info & 0xff, sn, add))
        t = sec['.text']
        self.tbase, self.toff, self.tsize = t[3], t[4], t[5]

    def sections(self):
        out = {}
        for n, x in self.secs:
            if not n: continue
            body = self.d[x[4]:x[4] + x[5]] if x[1] != 8 else b''
            out[n] = (x[5], sha(body)[:16])
        return out

    def functions(self):
        """{name: masked bytes} -- relocated bytes blanked"""
        relocated = set()
        for sec, off, typ, sn, add in self.rel:
            if sec == '.rela.text':
                o = off - self.tbase
                # the relocated field: bl = 4 bytes from the insn start, lea = the 4-byte immediate at +2
                for k in range(-2, 6): relocated.add(o + k)
        code = bytearray(self.d[self.toff:self.toff + self.tsize])
        for o in relocated:
            if 0 <= o < len(code): code[o] = 0
        out = {}
        for n, v, sz, typ, shn in self.syms:
            if typ == 2 and shn and sz:
                o = v - self.tbase
                out[n] = (bytes(code[o:o + sz]), self.d[self.toff + o:self.toff + o + sz])
        return out

    def imports(self): return sorted(n for n, v, sz, t, shn in self.syms if n and not shn)
    def exports(self): return sorted(n for n, v, sz, t, shn in self.syms if n and shn)

def stamp(d):
    m = re.search(rb'(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-9]\d \d{4}\x00\d\d:\d\d:\d\d', d)
    return m.group(0).replace(b'\0', b' ').decode() if m else None

def strings(d, n=4):
    return set(m.group(0).decode() for m in re.finditer(rb'[\x20-\x7e]{%d,}' % n, d))

def ident(path):
    d = open(path, 'rb').read()
    info = {"size": len(d), "sha256": sha(d)}
    if d[:4] == b'\x7fELF':
        e = Elf(d)
        info["kind"] = "VideoCore ELF library (e_machine 0x%x)" % struct.unpack('<H', d[18:20])[0]
        info["functions"] = sum(1 for s in e.syms if s[3] == 2 and s[4])
        info["imports"] = len(e.imports())
        info["relocations"] = len(e.rel)
        c = dict(e.secs).get('.comment')
        if c: info["comment"] = d[c[4]:c[4] + c[5]].replace(b'\0', b' ').decode('latin-1').strip()
    else:
        s = stamp(d)
        cmds = [g for g in HOST_GENCMDS if g.encode() + b'\0' in d or g.encode() + b' ' in d]
        info["kind"] = "VideoCore OS image" if b'vmcs_' in d else "data / other image"
        if s: info["build"] = s
        if cmds: info["host gencmds present"] = "%d of %d" % (len(cmds), len(HOST_GENCMDS))
    return info

def diff_file(name, pa, pb, out):
    a, b = open(pa, 'rb').read(), open(pb, 'rb').read()
    if a == b:
        out.append('%-22s identical (%s)' % (name, sha(a)[:16])); return 'identical'
    out.append('%-22s DIFFERENT  %d -> %d bytes' % (name, len(a), len(b)))
    if a[:4] == b'\x7fELF' and b[:4] == b'\x7fELF':
        ea, eb = Elf(a), Elf(b)
        sa, sb = ea.sections(), eb.sections()
        for s in sorted(set(sa) | set(sb)):
            if sa.get(s) != sb.get(s):
                out.append('    section %-14s %s -> %s' % (s, sa.get(s, ('-',))[0], sb.get(s, ('-',))[0]))
        xa, xb = set(ea.exports()), set(eb.exports())
        ia, ib = set(ea.imports()), set(eb.imports())
        for lab, x in (('exports added', xb - xa), ('exports removed', xa - xb), ('imports added', ib - ia), ('imports removed', ia - ib)):
            if x: out.append('    %s: %s' % (lab, ', '.join(sorted(x))))
        ra = collections.Counter((s, t) for s, o, t, n, ad in ea.rel); rb = collections.Counter((s, t) for s, o, t, n, ad in eb.rel)
        if ra != rb:
            out.append('    relocations: %d -> %d (%s)' % (len(ea.rel), len(eb.rel), ', '.join(
                '%s type %d: %d -> %d' % (k[0], k[1], ra.get(k, 0), rb.get(k, 0)) for k in sorted(set(ra) | set(rb)) if ra.get(k) != rb.get(k))))
        fa, fb = ea.functions(), eb.functions()
        same = moved = 0; changed = []
        for f in sorted(set(fa) & set(fb)):
            if fa[f][1] == fb[f][1]: same += 1
            elif fa[f][0] == fb[f][0]: moved += 1
            else: changed.append('%s (%d -> %d bytes)' % (f, len(fa[f][1]), len(fb[f][1])))
        out.append('    functions: %d identical, %d same code (relocations only), %d code changed, %d new, %d gone' % (
            same, moved, len(changed), len(set(fb) - set(fa)), len(set(fa) - set(fb))))
        for c in changed: out.append('      changed: ' + c)
        for f in sorted(set(fb) - set(fa)): out.append('      new: ' + f)
        for f in sorted(set(fa) - set(fb)): out.append('      gone: ' + f)
        lost = [s for s in HOST_SYMBOLS.get(name, []) if s in xa and s not in xb]
        if lost: out.append('    !! symbols bcm-rockbox relies on are gone: ' + ', '.join(lost))
        return 'changed'
    # raw images
    out.append('    build stamp: %s -> %s' % (stamp(a), stamp(b)))
    ga = [g for g in HOST_GENCMDS if g.encode() in a]; gb = [g for g in HOST_GENCMDS if g.encode() in b]
    if ga != gb: out.append('    !! host gencmd strings: %s missing in B' % ', '.join(sorted(set(ga) - set(gb))))
    sa, sb = strings(a, 6), strings(b, 6)
    out.append('    strings (6+ chars): %d added, %d removed' % (len(sb - sa), len(sa - sb)))
    for s in sorted(sb - sa)[:25]: out.append('      + ' + s)
    for s in sorted(sa - sb)[:25]: out.append('      - ' + s)
    blk = 4096
    na = [sha(a[i:i + blk]) for i in range(0, len(a), blk)]; nb = [sha(b[i:i + blk]) for i in range(0, len(b), blk)]
    same = sum(1 for x, y in zip(na, nb) if x == y)
    moved = len(set(nb) & set(na)) - same
    out.append('    4 KB blocks: %d of %d at the same place, %d found elsewhere' % (same, len(nb), max(0, moved)))
    return 'changed'

def main(argv):
    if len(argv) < 3:
        print(__doc__); return 1
    cmd = argv[1]
    if cmd == 'id':
        files = load_set(argv[2])
        for n in sorted(files):
            i = ident(files[n])
            print('%s' % n)
            for k, v in i.items(): print('    %-22s %s' % (k, v))
    elif cmd == 'diff':
        fa, fb = load_set(argv[2]), load_set(argv[3])
        out = ['VideoCore firmware comparison', '  A = %s' % argv[2], '  B = %s' % argv[3], '']
        res = collections.Counter()
        for n in sorted(set(fa) | set(fb)):
            if n not in fa: out.append('%-22s only in B' % n); res['only B'] += 1; continue
            if n not in fb: out.append('%-22s only in A' % n); res['only A'] += 1; continue
            res[diff_file(n, fa[n], fb[n], out)] += 1
        out.append('')
        ga, gb = set_gencmds(fa), set_gencmds(fb)
        out.append('host commands bcm-rockbox sends, found in the set: A %d of %d, B %d of %d%s' % (
            len(ga), len(HOST_GENCMDS), len(gb), len(HOST_GENCMDS),
            ('  -- missing in B: ' + ', '.join(sorted(set(ga) - set(gb)))) if set(ga) - set(gb) else ''))
        out.append('summary: ' + ', '.join('%d %s' % (v, k) for k, v in sorted(res.items())))
        text = '\n'.join(out) + '\n'
        if '-o' in argv: open(argv[argv.index('-o') + 1], 'w').write(text)
        sys.stdout.write(text)
    elif cmd == 'check':
        files = load_set(argv[2])
        man = TESTED
        if '--manifest' in argv: man = json.load(open(argv[argv.index('--manifest') + 1]))
        print('checking %s against: %s' % (argv[2], man['name']))
        ok = True
        for n in sorted(set(files) | set(man.get('files', {})) | set(man.get('sizes', {}))):
            if n not in files:
                print('  %-22s missing' % n); ok = ok and n not in ('vmcs.bin', 'mplayer.vll', 'h264dec.vll', 'passthruhandler.vll'); continue
            d = open(files[n], 'rb').read()
            if n in man.get('files', {}):
                good = sha(d) == man['files'][n]
                print('  %-22s %s' % (n, 'tested (hash matches)' if good else 'NOT the tested file (hash differs)'))
            elif n in man.get('sizes', {}):
                good = len(d) == man['sizes'][n]
                print('  %-22s %s' % (n, 'size matches the tested file' if good else 'size differs from the tested file (%d)' % len(d)))
            else:
                good = True; print('  %-22s not part of the tested set' % n)
            if n in ('vmcs.bin', 'mplayer.vll', 'h264dec.vll', 'aacdec.vll', 'passthruhandler.vll') and not good: ok = False
        g = set_gencmds(files)
        print('  host commands found: %d of %d%s' % (len(g), len(HOST_GENCMDS), (' (not found: %s)' % ', '.join(x for x in HOST_GENCMDS if x not in g)) if len(g) < len(HOST_GENCMDS) else ''))
        print('result: %s' % ('a tested combination' if ok else 'UNTESTED combination -- compare it with `diff` first'))
        return 0 if ok else 2
    elif cmd == 'manifest':
        files = load_set(argv[2])
        json.dump({"name": argv[3], "files": {n: sha(open(p, 'rb').read()) for n, p in sorted(files.items())},
                   "sizes": {n: os.path.getsize(p) for n, p in sorted(files.items())}}, sys.stdout, indent=1)
        print()
    else:
        print(__doc__); return 1
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))

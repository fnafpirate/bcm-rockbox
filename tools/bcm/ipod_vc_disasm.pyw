#!/usr/bin/env python3
"""iPod VideoCore Extractor & Disassembler -- bcm-rockbox tools (standard-library Python 3.8+, Tkinter for the window).

Takes an iPod 5th generation / 5.5 generation ("iPod Video") firmware package (.ipsw), or the Firmware-* file inside
it, or a folder that already holds the VideoCore files, and:

  1. EXTRACTS the VideoCore files (vmcs.bin, RenderServer.bin and the .vll libraries: mplayer, h264dec, aacdec,
     mpg4dec, passthruhandler, slideshow, ...) from the "Resources" FAT12 volume inside the firmware, long file
     names intact, to the drive / folder you choose.  On an iPod that runs Rockbox (a drive with a .rockbox folder)
     they can go straight to /.rockbox/VideoCore, where the bcm-rockbox video player reads them.
  2. DISASSEMBLES every one of them for developers, into the folder you choose:
       summary.txt       sections, every symbol, imports / exports, every relocation, decode coverage
       listing.asm       the complete code: EVERY byte of the code section, instruction by instruction, with
                         function headers, block labels, branch / call targets named, relocations, string operands
       data.txt          every data section as hex + text, with the data symbols and relocated pointers marked
       strings.txt       every printable string with its address
       functions/*.txt   one file per function: blocks + CFG edges, callers / callees, register tracking, memory
                         accesses, reconstructed structures, goto-level pseudocode (and a Graphviz .dot CFG)
       callgraph.dot     who calls whom
     plus index.html (open it in a browser) and README.txt for the whole firmware.

ABOUT "100 %": every byte is listed and every instruction's LENGTH is known (16 / 32 / 48 bit, verified on all named
functions of Apple's libraries), so the listing never loses its place.  The VideoCore II instruction set is not
public: the instructions whose meaning has been worked out are shown as mnemonics (bl, lea, branches, ld / st, mov,
add, push / pop, ...); the others are shown with their exact encoding (.hw xxxx / op32 / op48), never guessed.
summary.txt says how many of each there are.

Usage:  double-click (window), or
        python ipod_vc_disasm.pyw --input iPod_25.1.3.ipsw --files-out E:\\ --dis-out D:\\vc_disasm [--rockbox]
Reads the input only; writes only under the two folders chosen.  Apple's files are not included with this tool.
"""
import sys, os, struct, bisect, re, collections, zipfile, hashlib, html, threading, time, string, json

# =====================================================================================================================
#  VideoCore II decoder and analysis (the same code as tools/bcm/vc2wb.py)
# =====================================================================================================================
# ------------------------------------------------------------------------------------------------------------ files
def sx(v, b):
    return v - (1 << b) if v & (1 << (b - 1)) else v

class Image:
    """an ELF .vll or a raw image; .code = bytes of the executable part, .base = its address"""
    def __init__(self, path, data=None):
        self.path = path
        self.d = data if data is not None else open(path, 'rb').read()
        self.elf = self.d[:4] == b'\x7fELF'
        self.syms, self.rel, self.secs = [], [], []
        if self.elf:
            self._elf()
        else:
            self.base, self.code = 0, self.d
        self.relat = {}
        for sec, off, typ, si, add in self.rel:
            if sec == '.rela.text':
                self.relat[off] = (typ, self.symname(si), add, si)
        self.names = {v: n for n, v, sz, info, shn in self.syms if info & 0xf == 2 and shn}
        self.dnames = {v: n for n, v, sz, info, shn in self.syms if info & 0xf == 1 and shn}

    def _elf(self):
        d = self.d
        shoff = struct.unpack('<I', d[0x20:0x24])[0]
        shnum, shstr = struct.unpack('<HH', d[0x30:0x34])
        sh = [struct.unpack('<10I', d[shoff + i * 40:shoff + i * 40 + 40]) for i in range(shnum)]
        so = sh[shstr][4]
        def nm(o):
            e = d.find(b'\0', so + o)
            return d[so + o:e].decode('latin-1')
        self.secs = [(nm(x[0]), x) for x in sh]
        sec = dict(self.secs)
        t = sec['.text']
        self.base, self.code = t[3], d[t[4]:t[4] + t[5]]
        ds, st = sec.get('.dynsym'), sec.get('.dynstr')
        if ds and st:
            for i in range(ds[5] // 16):
                n, v, sz, info, oth, shn = struct.unpack('<IIIBBH', d[ds[4] + i * 16:ds[4] + i * 16 + 16])
                e = d.find(b'\0', st[4] + n)
                self.syms.append((d[st[4] + n:e].decode('latin-1'), v, sz, info, shn))
        for name, x in self.secs:
            if name.startswith('.rela') and x[1] == 4:
                for i in range(x[5] // 12):
                    off, info, add = struct.unpack('<IIi', d[x[4] + i * 12:x[4] + i * 12 + 12])
                    self.rel.append((name, off, info & 0xff, info >> 8, add))

    def symname(self, i):
        return self.syms[i][0] if i < len(self.syms) else '?'

    def hw(self, a):
        o = a - self.base
        return struct.unpack('<H', self.code[o:o + 2])[0] if 0 <= o and o + 2 <= len(self.code) else 0

    def u32(self, a):
        o = a - self.base
        return struct.unpack('<I', self.code[o:o + 4])[0] if 0 <= o and o + 4 <= len(self.code) else 0

    def end(self):
        return self.base + len(self.code)

    def reloc_value(self, rl):
        """(address or None, name) a relocation stands for: a defined symbol + addend, an import by name, or the addend"""
        typ, nm, add, si = rl
        if si and si < len(self.syms):
            n, v, sz, info, shn = self.syms[si]
            if shn and (info & 0xf) == 3:              # section symbol: this linker puts the full address in the addend
                return add, self.name_of(add) or self.section_of(add) or ''
            if shn: return v + add, n
            return None, n
        return add, self.name_of(add) or self.section_of(add) or ''

    def name_of(self, a):
        return self.names.get(a) or self.dnames.get(a)

    def section_of(self, a):
        for n, x in self.secs:
            if x[3] and x[3] <= a < x[3] + x[5]:
                return n
        return None

# ------------------------------------------------------------------------------------------------------ decoder
CC = {0: 'eq', 1: 'ne', 2: 'cs', 3: 'cc', 4: 'mi', 5: 'pl', 6: 'vs', 7: 'vc', 8: 'hi', 9: 'ls', 10: 'ge', 11: 'lt',
      12: 'gt', 13: 'le', 14: 'al', 15: 'nv'}
COP = {'eq': '==', 'ne': '!=', 'cs': '>=u', 'cc': '<u', 'hi': '>u', 'ls': '<=u', 'ge': '>=', 'lt': '<', 'gt': '>',
       'le': '<=', 'mi': '<0', 'pl': '>=0'}

class Insn:
    __slots__ = ('a', 'n', 'kind', 'text', 'tg', 'op', 'rd', 'rs', 'imm', 'mem', 'cond', 'table')
    def __init__(self, a, n, kind, text, **k):
        self.a, self.n, self.kind, self.text = a, n, kind, text
        self.tg = k.get('tg'); self.op = k.get('op'); self.rd = k.get('rd'); self.rs = k.get('rs')
        self.imm = k.get('imm'); self.mem = k.get('mem'); self.cond = k.get('cond'); self.table = k.get('table')

def ilen(h):
    t = h >> 8
    if t == 0x2f: return 6
    if 0x10 <= t <= 0x1f and (h & 0x80): return 4
    if t in (0x1e, 0x1f): return 4
    if 0x28 <= t <= 0x2e: return 4
    return 2

def decode(img, a, fend=None):
    h = img.hw(a)
    if (h & 0xf080) == 0x1080:
        h1 = img.hw(a + 2)
        tg = a + 2 * sx(((h >> 8) & 15) << 23 | h1 << 7 | (h & 0x7f), 27)
        return Insn(a, 4, 'call', 'bl     0x%x' % tg, tg=tg, op='bl')
    if (h >> 8) == 0x2f:
        imm = img.u32(a + 2)
        if ((h >> 4) & 15) == 6:
            return Insn(a, 6, 'op', 'mov    r%d, #0x%x' % (h & 15, imm), op='movi', rd=h & 15, imm=imm)
        if (h & 0xffe0) == 0x2f20:
            r = h & 0x1f
            if imm >> 27 == 31:
                v = a + sx(imm & 0x7ffffff, 27)
                return Insn(a, 6, 'op', 'lea    r%d, 0x%x' % (r, v), op='lea', rd=r, imm=v)
            if imm >> 27 == 24:
                v = imm & 0x7ffffff
                return Insn(a, 6, 'op', 'lea    r%d, 0x%x (abs)' % (r, v), op='lea', rd=r, imm=v)
        return Insn(a, 6, 'unk', 'op48   %04x %08x' % (h, imm), op='?')
    if (h >> 12) == 0:
        h1 = img.hw(a + 2)
        if (h1 >> 14) == 3:
            c = CC[(h >> 8) & 15]; i = sx((h >> 4) & 15, 4); r = h & 15; f = (h1 >> 8) & 0x3f
            tg = a + 2 * sx(h1 & 0xff, 8)
            add = (' r%d+=%d;' % (r, i)) if i else ''
            return Insn(a, 4, 'cjump', 'b%-5s%s r%d ? op%02x -> 0x%x' % (c, add, r, f, tg), tg=tg, op='bcc', rd=r,
                        imm=i, cond=(c, f))
    if (h >> 8) in (0x1e, 0x1f) and not (h & 0x80):
        h1 = img.hw(a + 2)
        tg = a + 2 * sx(h1 << 7 | (h & 0x7f), 23)
        return Insn(a, 4, 'jump', 'b%s    0x%x' % ('' if (h >> 8) == 0x1e else '.f?', tg), tg=tg, op='b')
    if (h >> 8) in (0x4c, 0x4d):
        wide = (h >> 8) == 0x4d
        nxt, tgs, i, lo = a + 2, [], 0, None
        limit = fend if fend else nxt + 512
        while i < 256:
            p = nxt + (2 * i if wide else i)
            if lo is not None and p >= lo: break
            if p >= limit: break
            e = img.hw(p) if wide else img.code[p - img.base] if 0 <= p - img.base < len(img.code) else 0
            t = nxt + 2 * e
            if t <= p or (fend and t >= fend): break
            tgs.append(t); lo = t if lo is None else min(lo, t); i += 1
        size = (2 * len(tgs) if wide else (len(tgs) + 1) & ~1)
        return Insn(a, 2 + size, 'switch', '%s    r%d  [%d cases]' % ('tbh' if wide else 'tbb', h & 15, len(tgs)),
                    op='tb', rd=h & 15, table=tgs)
    if (h >> 8) == 0x3e:
        tg = a + 2 * sx(h & 0xff, 8)
        return Insn(a, 2, 'jump', 'b      0x%x' % tg, tg=tg, op='b')
    if (h & 0xfff0) == 0xf0a0: return Insn(a, 2, 'op', 'push   r6-r%d, lr' % (6 + (h & 15)), op='push', imm=h & 15)
    if h == 0xf0ef: return Insn(a, 2, 'op', 'push   lr', op='push', imm=-1)
    if (h & 0xfff0) == 0xe0a0: return Insn(a, 2, 'ret', 'pop    r6-r%d, pc' % (6 + (h & 15)), op='pop')
    if h == 0xe0ef: return Insn(a, 2, 'ret', 'pop    pc', op='pop')
    if h == 0x4430: return Insn(a, 2, 'ret', 'rts', op='rts')
    if (h >> 12) == 0xe:
        return Insn(a, 2, 'op', 'ld     r%d, [r%d+0x%x]' % (h & 15, (h >> 4) & 15, 4 * ((h >> 8) & 15)), op='ld',
                    rd=h & 15, rs=(h >> 4) & 15, mem=(4 * ((h >> 8) & 15), 4))
    if (h >> 12) == 0xf:
        return Insn(a, 2, 'op', 'st     r%d, [r%d+0x%x]' % (h & 15, (h >> 4) & 15, 4 * ((h >> 8) & 15)), op='st',
                    rd=h & 15, rs=(h >> 4) & 15, mem=(4 * ((h >> 8) & 15), 4))
    if (h >> 8) == 0xb8: return Insn(a, 2, 'op', 'ld     r%d, [r%d]' % (h & 15, (h >> 4) & 15), op='ld', rd=h & 15, rs=(h >> 4) & 15, mem=(0, 4))
    if (h >> 8) == 0xb9: return Insn(a, 2, 'op', 'st     r%d, [r%d]' % (h & 15, (h >> 4) & 15), op='st', rd=h & 15, rs=(h >> 4) & 15, mem=(0, 4))
    if (h >> 8) == 0xcc: return Insn(a, 2, 'op', 'add    r%d, r%d' % (h & 15, (h >> 4) & 15), op='add', rd=h & 15, rs=(h >> 4) & 15)
    if (h >> 8) == 0xcb: return Insn(a, 2, 'op', 'rsub   r%d, r%d' % (h & 15, (h >> 4) & 15), op='rsub', rd=h & 15, rs=(h >> 4) & 15)
    if (h >> 8) == 0x6c: return Insn(a, 2, 'op', 'add    r%d, #%d' % (h & 15, (h >> 4) & 15), op='addi', rd=h & 15, imm=(h >> 4) & 15)
    if (h >> 8) == 0x68: return Insn(a, 2, 'op', 'lsr    r%d, #%d' % (h & 15, (h >> 4) & 15), op='lsr', rd=h & 15, imm=(h >> 4) & 15)
    if (h >> 8) == 0x2c:
        off = img.hw(a + 2)
        return Insn(a, 4, 'op', 'ld?    r%d, [r%d+0x%x]' % (h & 15, (h >> 4) & 15, off), op='ld', rd=h & 15, rs=(h >> 4) & 15, mem=(off, '16/32?'))
    if (h >> 8) == 0x28:
        off = img.hw(a + 2)
        return Insn(a, 4, 'op', 'mem28  r%d, [r%d+0x%x]' % (h & 15, (h >> 4) & 15, off), op='m28', rd=h & 15, rs=(h >> 4) & 15, mem=(off, '16/32?'))
    if (h >> 8) == 0x2e and ((h >> 4) & 15) == 5:
        v = sx(img.hw(a + 2), 16)                       # v96: pairs "ff00" after push / "0100" before pop: the stack frame
        return Insn(a, 4, 'op', 'add    r%d, #%d   (r25 = sp)' % (16 + (h & 15), v), op='addhi', rd=16 + (h & 15), imm=v)
    if (h >> 8) == 0x2e and ((h >> 4) & 15) == 6:
        v = img.hw(a + 2)
        return Insn(a, 4, 'op', 'mov    r%d, #0x%x' % (h & 15, v), op='movi', rd=h & 15, imm=v)
    if (h >> 8) == 0xce: return Insn(a, 2, 'op', 'mov    r%d, r%d' % (h & 15, (h >> 4) & 15), op='mov', rd=h & 15, rs=(h >> 4) & 15)
    if (h >> 8) == 0x6e: return Insn(a, 2, 'op', 'mov    r%d, #%d' % (h & 15, (h >> 4) & 15), op='movi', rd=h & 15, imm=(h >> 4) & 15)
    l = ilen(h)
    if l == 4: return Insn(a, 4, 'unk', 'op32   %04x %04x' % (h, img.hw(a + 2)), op='?')
    return Insn(a, 2, 'unk', '.hw    %04x' % h, op='?')

def reloc_in(img, ins):
    for o in range(ins.a, ins.a + ins.n):
        if o in img.relat:
            return img.relat[o]
    return None

# -------------------------------------------------------------------------------------------- function discovery
def discover(img):
    """returns sorted [(start, end, name, how)]"""
    starts = {}
    if img.elf:
        for n, v, sz, info, shn in img.syms:
            if info & 0xf == 2 and shn and sz:
                starts[v] = (v + sz, n, 'symbol')
    # call targets + pushes after returns, from a linear sweep
    a, prev_ret = img.base, True
    calls = collections.Counter()
    while a < img.end():
        ins = decode(img, a)
        if ins.kind == 'call' and img.base <= ins.tg < img.end() and not reloc_in(img, ins):
            calls[ins.tg] += 1
        if ins.op == 'push' and prev_ret and a not in starts:
            starts.setdefault(a, (None, None, 'prologue after a return'))
        prev_ret = ins.kind == 'ret' or (ins.kind == 'jump')
        a += ins.n
    for t, n in calls.items():
        if t in starts: continue
        if img.elf and any(s <= t < e for s, (e, nm, how) in starts.items() if e):
            continue                                             # inside a known symbol: a local label, not a function
        if decode(img, t).op == 'push' or img.elf:
            starts[t] = (None, None, 'bl target (%d calls)' % n)
    keys = sorted(starts)
    out = []
    for i, s in enumerate(keys):
        e, n, how = starts[s]
        if e is None:
            e = keys[i + 1] if i + 1 < len(keys) else img.end()
        out.append((s, e, n or img.name_of(s) or 'sub_%x' % s, how))
    return out

# -------------------------------------------------------------------------------------------------- per function
class Func:
    strict = False                                       # --strict: an unknown instruction forgets every register
    def __init__(self, img, start, end, name):
        self.img, self.start, self.end, self.name = img, start, end, name
        self.ins = []
        a = start
        while a < end:
            i = decode(img, a, end)
            self.ins.append(i); a += i.n
        self.at = {i.a: i for i in self.ins}
        self._blocks()

    def _blocks(self):
        leaders = {self.start}
        for i in self.ins:
            if i.kind in ('jump', 'cjump') and self.start <= i.tg < self.end: leaders.add(i.tg)
            if i.kind == 'switch':
                for t in i.table: leaders.add(t)
            if i.kind in ('jump', 'cjump', 'ret', 'switch'): leaders.add(i.a + i.n)
        leaders = sorted(x for x in leaders if x in self.at)
        self.blocks = collections.OrderedDict()
        for k, s in enumerate(leaders):
            e = leaders[k + 1] if k + 1 < len(leaders) else self.end
            body = [i for i in self.ins if s <= i.a < e]
            self.blocks[s] = body
        self.succ = {}
        for s, body in self.blocks.items():
            last = body[-1] if body else None
            nxt = last.a + last.n if last else s
            out = []
            if last is None: pass
            elif last.kind == 'jump':
                out.append((last.tg, 'goto' if self.start <= last.tg < self.end else 'tail'))
            elif last.kind == 'cjump':
                out += [(last.tg, 'taken'), (nxt, 'fall')]
            elif last.kind == 'switch':
                out += [(t, 'case %d' % n) for n, t in enumerate(last.table)]
            elif last.kind == 'ret':
                pass
            else:
                out.append((nxt, 'fall'))
            self.succ[s] = [(t, k) for t, k in out if t in self.blocks or k == 'tail']
        self.pred = collections.defaultdict(list)
        for s, o in self.succ.items():
            for t, k in o:
                self.pred[t].append(s)

    # ---- register tracking: values are ('c', int) | ('g', addr) | ('a', n, off) | ('f', n, base_off, off)
    def track(self):
        self.state_in, notes = {}, {}
        self.mem = []                                    # (insn addr, 'ld'/'st', value-of-base, off, width, reg)
        entry = {r: ('a', r, 0) for r in range(6)}
        work, done = [self.start], set()
        out_states = {}
        order = list(self.blocks)
        for s in order:                                  # address order, one pass, entry = merge of predecessors seen
            preds = [out_states[p] for p in self.pred.get(s, []) if p in out_states]
            if s == self.start: st = dict(entry)
            elif preds:
                st = dict(preds[0])
                for p in preds[1:]:
                    for r in list(st):
                        if p.get(r) != st[r]: del st[r]
            else: st = {}
            self.state_in[s] = dict(st)
            for i in self.blocks[s]:
                notes[i.a] = self._step(i, st)
            out_states[s] = st
        self.notes = notes

    def _step(self, i, st):
        def val(r): return st.get(r)
        note = ''
        if i.op == 'movi': st[i.rd] = ('c', i.imm); note = '= 0x%x' % i.imm
        elif i.op == 'lea':
            tgt, nm = lea_target(self.img, i)
            if tgt is None:
                st[i.rd] = ('i', nm); note = '= &%s (import)' % nm
            else:
                st[i.rd] = ('g', tgt); note = '= &0x%x %s' % (tgt, nm)
                s = string_at(self.img, tgt)
                if s: note += ' "%s"' % s
        elif i.op == 'mov':
            v = val(i.rs)
            if v is not None: st[i.rd] = v; note = '= ' + fmt_val(v)
            else: st.pop(i.rd, None)
        elif i.op == 'addi':
            v = val(i.rd)
            if v and v[0] == 'c': st[i.rd] = ('c', (v[1] + i.imm) & 0xffffffff)
            elif v and v[0] == 'g': st[i.rd] = ('g', v[1] + i.imm)
            elif v and v[0] == 'a': st[i.rd] = ('a', v[1], v[2] + i.imm)
            else: st.pop(i.rd, None)
            if i.rd in st: note = '= ' + fmt_val(st[i.rd])
        elif i.op == 'add':
            a, b = val(i.rd), val(i.rs)
            if a and b and a[0] == 'c' and b[0] == 'c': st[i.rd] = ('c', (a[1] + b[1]) & 0xffffffff)
            elif a and b and a[0] in 'ga' and b[0] == 'c':
                st[i.rd] = ('g', a[1] + b[1]) if a[0] == 'g' else ('a', a[1], a[2] + b[1])
            elif a and b and b[0] in 'ga' and a[0] == 'c':
                st[i.rd] = ('g', b[1] + a[1]) if b[0] == 'g' else ('a', b[1], b[2] + a[1])
            else: st.pop(i.rd, None)
        elif i.op in ('rsub', 'lsr'):
            st.pop(i.rd, None)
        elif i.op in ('ld', 'st', 'm28'):
            base = val(i.rs); off, width = i.mem
            kind = {'ld': 'ld', 'st': 'st', 'm28': 'ld/st?'}[i.op]
            self.mem.append((i.a, kind, base, off, width, i.rd))
            if base: note = '%s %s' % ('<-' if i.op == 'ld' else '->', fmt_mem(self.img, base, off))
            if i.op == 'ld' or i.op == 'm28':
                if base and base[0] == 'a': st[i.rd] = ('f', base[1], base[2] + off, 0)
                elif base and base[0] == 'g' and i.op == 'ld': st.pop(i.rd, None)
                else: st.pop(i.rd, None)
        elif i.kind == 'call':
            for r in range(6): st.pop(r, None)
        elif i.kind == 'unk':
            if self.strict: st.clear()
            else:                                        # the destination field of every decoded form is a low nibble
                h = self.img.hw(i.a)
                st.pop(h & 15, None); st.pop((h >> 4) & 15, None)
        elif i.op == 'addhi':
            st.pop(i.rd, None)
        elif i.op == 'bcc' and i.imm:
            v = val(i.rd)
            if v and v[0] == 'c': st[i.rd] = ('c', (v[1] + i.imm) & 0xffffffff)
            else: st.pop(i.rd, None)
        return note

    def structures(self):
        """{('arg', n) | ('field', n, off): {offset: set(kinds/widths)}}"""
        out = collections.defaultdict(lambda: collections.defaultdict(set))
        for a, kind, base, off, width, reg in self.mem:
            if not base: continue
            if base[0] == 'a':
                out[('arg', base[1])][base[2] + off].add('%s %s' % (kind, width))
            elif base[0] == 'f':
                out[('field', base[1], base[2])][base[3] + off].add('%s %s' % (kind, width))
        return out

    def globals_used(self):
        g = collections.defaultdict(set)
        for a, kind, base, off, width, reg in self.mem:
            if base and base[0] == 'g':
                g[base[1] + off].add(kind)
        for i in self.ins:
            if i.op == 'lea':
                t, nm = lea_target(self.img, i)
                g.setdefault(t if t is not None else nm, set()).add('address taken')
        return g

def lea_target(img, i):
    rl = reloc_in(img, i)
    if rl: return img.reloc_value(rl)
    return i.imm, img.name_of(i.imm) or img.section_of(i.imm) or ''

def fmt_val(v):
    if v[0] == 'i': return '&' + v[1]
    if v[0] == 'c': return '0x%x' % v[1]
    if v[0] == 'g': return '&0x%x' % v[1]
    if v[0] == 'a': return 'arg%d%s' % (v[1], '+0x%x' % v[2] if v[2] else '')
    if v[0] == 'f': return 'arg%d->f_%x%s' % (v[1], v[2], '+0x%x' % v[3] if v[3] else '')
    return '?'

def fmt_mem(img, base, off):
    if base[0] == 'a': return 'arg%d->f_%x' % (base[1], base[2] + off)
    if base[0] == 'f': return 'arg%d->f_%x->f_%x' % (base[1], base[2], base[3] + off)
    if base[0] in 'gc':
        a = base[1] + off
        return '[0x%x%s]' % (a, ' ' + (img.name_of(a) or img.section_of(a) or '') if img.elf else '')
    return '?'

def string_at(img, a):
    if not img.elf:
        o = a
        d = img.d
    else:
        sec = None
        for n, x in img.secs:
            if x[3] and x[3] <= a < x[3] + x[5] and x[1] == 1:
                sec = x
        if not sec: return None
        o = sec[4] + a - sec[3]; d = img.d
    if not (0 <= o < len(d)): return None
    e = d.find(b'\0', o, o + 80)
    if e - o < 3: return None
    s = d[o:e]
    if all(32 <= c < 127 or c in (9, 10) for c in s):
        return s.decode().replace('\n', '\\n')
    return None

# -------------------------------------------------------------------------------------------------- pseudocode
def pseudo(f):
    out = []
    for s, body in f.blocks.items():
        out.append('L_%x:' % s)
        for i in body:
            st = f.notes.get(i.a, '')
            c = None
            if i.op == 'movi': c = 'r%d = 0x%x;' % (i.rd, i.imm)
            elif i.op == 'lea':
                t, nm = lea_target(f.img, i)
                s2 = string_at(f.img, t) if t is not None else None
                lab = nm if nm and not nm.startswith('.') else 'data_%x' % t
                c = 'r%d = &%s;' % (i.rd, lab) + (' /* "%s" */' % s2 if s2 else '')
            elif i.op == 'addhi': c = 'r%d += %d;%s' % (i.rd, i.imm, '   /* stack frame */' if i.rd == 25 else '')
            elif i.op == 'mov': c = 'r%d = r%d;' % (i.rd, i.rs)
            elif i.op == 'add': c = 'r%d += r%d;' % (i.rd, i.rs)
            elif i.op == 'addi': c = 'r%d += %d;' % (i.rd, i.imm)
            elif i.op == 'rsub': c = 'r%d = r%d - r%d;' % (i.rd, i.rs, i.rd)
            elif i.op == 'lsr': c = 'r%d >>= %d;' % (i.rd, i.imm)
            elif i.op in ('ld', 'st', 'm28'):
                m = st.split(' ', 1)[1] if st.startswith(('<-', '->')) else '*(u32 *)(r%d + 0x%x)' % (i.rs, i.mem[0])
                c = {'ld': 'r%d = %s;' % (i.rd, m), 'st': '%s = r%d;' % (m, i.rd),
                     'm28': 'r%d <-> %s;   /* 0x28: load or store, unresolved */' % (i.rd, m)}[i.op]
            elif i.kind == 'call':
                rl = reloc_in(f.img, i)
                nm = rl[1] if rl and rl[1] != '?' else f.img.name_of(i.tg) or 'sub_%x' % i.tg
                c = 'r0 = %s(r0, r1, r2, r3, r4, r5);' % nm
            elif i.op == 'bcc':
                cnd, fo = i.cond
                pre = 'r%d += %d; ' % (i.rd, i.imm) if i.imm else ''
                c = '%sif (r%d %s op_%02x) goto L_%x;' % (pre, i.rd, COP.get(cnd, cnd), fo, i.tg)
            elif i.kind == 'jump':
                c = 'goto L_%x;' % i.tg if f.start <= i.tg < f.end else 'return %s(...);   /* tail call */' % (f.img.name_of(i.tg) or 'sub_%x' % i.tg)
            elif i.kind == 'switch':
                c = 'switch (r%d) { %s }' % (i.rd, ' '.join('case %d: goto L_%x;' % (n, t) for n, t in enumerate(i.table)))
            elif i.kind == 'ret': c = 'return r0;'
            elif i.op == 'push': c = '/* prologue: save r6.. and lr */'
            else: c = '__asm("%s");' % i.text.strip()
            out.append('    %-60s // %06x' % (c, i.a))
    return '\n'.join(out)

# ------------------------------------------------------------------------------------------------------- output
def describe(img, f, callers=None, dot=None):
    f.track()
    L = []
    L.append('==== %s  0x%x..0x%x  (%d bytes, %d instructions, %d blocks)' % (f.name, f.start, f.end, f.end - f.start,
                                                                            len(f.ins), len(f.blocks)))
    unk = sum(1 for i in f.ins if i.kind == 'unk')
    L.append('decoded: %d of %d instructions (%d unknown)' % (len(f.ins) - unk, len(f.ins), unk))
    if callers is not None:
        L.append('called from: %s' % (', '.join(sorted(set(callers))) or '(no direct call found)'))
    callees = []
    for i in f.ins:
        if i.kind == 'call':
            rl = reloc_in(img, i)
            callees.append(rl[1] if rl and rl[1] != '?' else img.name_of(i.tg) or 'sub_%x' % i.tg)
    L.append('calls: %s' % (', '.join(sorted(set(callees))) or '-'))
    L.append('')
    L.append('-- disassembly (block leaders marked, register / memory notes on the right)')
    for s, body in f.blocks.items():
        succ = ', '.join('%s L_%x' % (k, t) for t, k in f.succ.get(s, []))
        L.append('L_%x:%s' % (s, ('   -> ' + succ) if succ else ''))
        for i in body:
            raw = img.code[i.a - img.base:i.a - img.base + min(i.n, 6)].hex(' ')
            rl = reloc_in(img, i)
            r = '   <reloc %d %s%+d>' % rl[:3] if rl else ''
            n = f.notes.get(i.a, '')
            t = i.text
            if rl and i.op == 'lea':                    # the relocated operand, not the placeholder bytes
                tg, nm = lea_target(img, i)
                t = 'lea    r%d, %s' % (i.rd, nm if tg is None else '0x%x' % tg)
            elif rl and i.kind == 'call' and rl[1] not in ('?', ''):
                t = 'bl     %s' % rl[1]
            L.append('  %06x: %-18s %-42s %s%s' % (i.a, raw, t, n, r))
    L.append('')
    L.append('-- memory accesses')
    if not f.mem: L.append('  none')
    for a, kind, base, off, width, reg in f.mem:
        L.append('  %06x %-6s %-40s width %s, register r%d' % (a, kind, fmt_mem(img, base, off) if base else '[r? + 0x%x] (base unknown)' % off, width, reg))
    L.append('')
    L.append('-- reconstructed structures (fields reached through argument registers)')
    ss = f.structures()
    if not ss: L.append('  none')
    for key in sorted(ss, key=str):
        nm = 'struct arg%d' % key[1] if key[0] == 'arg' else 'struct arg%d->f_%x' % (key[1], key[2])
        L.append('  %s {' % nm)
        for off in sorted(ss[key]):
            L.append('      +0x%03x  %s' % (off, ', '.join(sorted(ss[key][off]))))
        L.append('  }')
    L.append('')
    g = f.globals_used()
    L.append('-- globals / data referenced')
    if not g: L.append('  none')
    for a in sorted(g, key=lambda k: (isinstance(k, str), k if isinstance(k, int) else 0, str(k))):
        if isinstance(a, str):
            L.append('  %-23s %s (import)' % (a, ', '.join(sorted(g[a])))); continue
        s = string_at(img, a)
        L.append('  0x%06x %-14s %s%s' % (a, img.name_of(a) or img.section_of(a) or '', ', '.join(sorted(g[a])), ('  "%s"' % s) if s else ''))
    L.append('')
    L.append('-- pseudocode (goto level)')
    L.append('%s(arg0, arg1, arg2, arg3, arg4, arg5)\n{' % f.name)
    L.append(pseudo(f))
    L.append('}')
    if dot:
        with open(dot, 'w') as o:
            o.write(cfg_dot(f))
    return '\n'.join(L)

def cfg_dot(f):
    L = ['digraph "%s" {' % f.name, '  node [shape=box fontname="monospace" fontsize=9];']
    for s, body in f.blocks.items():
        txt = '\\l'.join('%06x %s' % (i.a, i.text.replace('"', "'")) for i in body[:14])
        if len(body) > 14: txt += '\\l... %d more' % (len(body) - 14)
        L.append('  b%x [label="L_%x\\l%s\\l"];' % (s, s, txt))
        for t, k in f.succ.get(s, []):
            if t in f.blocks:
                L.append('  b%x -> b%x [label="%s"];' % (s, t, k))
    L.append('}')
    return '\n'.join(L) + '\n'

def xref_table(img, funcs):
    starts = [s for s, e, n, h in funcs]
    def owner(a):
        k = bisect.bisect_right(starts, a) - 1
        return funcs[k][2] if k >= 0 and a < funcs[k][1] else '?'
    callers = collections.defaultdict(list)
    refs = collections.defaultdict(list)
    for s, e, n, h in funcs:
        a = s
        while a < e:
            i = decode(img, a, e)
            if i.kind == 'call':
                rl = reloc_in(img, i)
                t = rl[1] if rl and rl[1] != '?' else img.name_of(i.tg) or 'sub_%x' % i.tg
                callers[t].append(n)
            if i.op == 'lea':
                t, nm = lea_target(img, i)
                refs[t if t is not None else nm].append(n)
            a += i.n
    return callers, refs, owner

def find(img, funcs, key):
    for s, e, n, h in funcs:
        if n == key: return s, e, n
    a = int(key, 0)
    for s, e, n, h in funcs:
        if s <= a < e: return s, e, n
    raise SystemExit('no function %s' % key)


# =====================================================================================================================
#  extraction
# =====================================================================================================================
TOOL_VERSION = '1.0'

class ToolError(Exception):
    pass

class Fat12:
    """the firmware's "Resources" volume (FAT12, OEM "MTOOL"), long file names supported"""
    def __init__(self, img, off):
        self.img, self.off = img, off
        b = img[off:off + 512]
        self.bps = struct.unpack_from('<H', b, 11)[0]
        self.spc = b[13]
        self.rsv = struct.unpack_from('<H', b, 14)[0]
        self.nfat = b[16]
        self.nroot = struct.unpack_from('<H', b, 17)[0]
        self.spf = struct.unpack_from('<H', b, 22)[0]
        fat0 = off + self.rsv * self.bps
        self.fat = img[fat0:fat0 + self.spf * self.bps]
        self.root = off + (self.rsv + self.nfat * self.spf) * self.bps
        self.data = self.root + self.nroot * 32
        self.csize = self.spc * self.bps

    def _next(self, c):
        o = c * 3 // 2
        if o + 1 >= len(self.fat): return 0xFFF
        v = self.fat[o] | (self.fat[o + 1] << 8)
        return (v >> 4) if (c & 1) else (v & 0xFFF)

    def read(self, c, size=None):
        out, seen = [], set()
        while 2 <= c < 0xFF8 and c not in seen:
            seen.add(c)
            o = self.data + (c - 2) * self.csize
            out.append(self.img[o:o + self.csize])
            c = self._next(c)
        buf = b''.join(out)
        return buf if size is None else buf[:size]

    @staticmethod
    def _parse(data):
        ents, lfn = [], []
        for i in range(0, len(data) - 31, 32):
            e = data[i:i + 32]
            if e[0] == 0: break
            if e[0] == 0xE5: lfn = []; continue
            if e[11] == 0x0F:
                s = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16le', 'replace')
                lfn.insert(0, s.split('\x00')[0].split('\uffff')[0]); continue
            if e[11] & 0x08: lfn = []; continue                       # volume label
            base = e[:8].decode('latin1').rstrip(); ext = e[8:11].decode('latin1').rstrip()
            short = base + ('.' + ext if ext else '')
            ents.append({'name': ''.join(lfn) or short, 'dir': bool(e[11] & 0x10),
                         'cluster': struct.unpack_from('<H', e, 26)[0], 'size': struct.unpack_from('<I', e, 28)[0]})
            lfn = []
        return ents

    def walk(self, ents=None, prefix='', depth=0):
        if ents is None: ents = self._parse(self.img[self.root:self.root + self.nroot * 32])
        for e in ents:
            if e['name'] in ('.', '..'): continue
            p = prefix + '/' + e['name']
            if e['dir']:
                if depth < 16: yield from self.walk(self._parse(self.read(e['cluster'])), p, depth + 1)
            else:
                yield p, e

def find_fat12(img):
    for off in range(0, len(img) - 512, 512):
        b = img[off:off + 512]
        if b[510:512] == b'\x55\xAA' and b[3:8] == b'MTOOL' and struct.unpack_from('<H', b, 11)[0] == 512 \
                and b[13] in (1, 2, 4, 8, 16) and b[16] in (1, 2):
            yield off

def firmware_images(path):
    """[(name, bytes)] -- every Firmware-* member of an .ipsw, or the file itself"""
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if os.path.basename(n).startswith('Firmware-') and not n.endswith('/')]
            if not names:
                raise ToolError('%s has no Firmware-* file: not an iPod firmware package' % os.path.basename(path))
            return [(os.path.basename(n), z.read(n)) for n in names]
    with open(path, 'rb') as f:
        return [(os.path.basename(path), f.read())]

def extract_videocore(img):
    """{'/Resources/VideoCore/...': bytes} from one firmware image"""
    out = {}
    for off in find_fat12(img):
        try:
            fs = Fat12(img, off)
            for p, e in fs.walk():
                if '/videocore/' in p.lower():
                    out[p] = fs.read(e['cluster'], e['size']) if e['size'] else b''
        except (IndexError, struct.error, UnicodeDecodeError):
            continue
        if out: break
    return out

def videocore_from_folder(folder):
    out = {}
    for dp, dn, fn in os.walk(folder):
        for f in fn:
            if f.lower().endswith(('.vll', '.bin')):
                p = os.path.join(dp, f)
                with open(p, 'rb') as h: d = h.read()
                if d[:4] == b'\x7fELF' or f.lower() in ('vmcs.bin', 'renderserver.bin'):
                    out['/' + os.path.relpath(p, folder).replace(os.sep, '/')] = d
    return out

# =====================================================================================================================
#  disassembly
# =====================================================================================================================
def safe_name(n):
    n = re.sub(r'[^A-Za-z0-9_.\-]', '_', n)
    return n[:80] or 'unnamed'

def build_stamp(d):
    m = re.search(rb'(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-9]\d \d{4}\x00\d\d:\d\d:\d\d', d)
    return m.group(0).replace(b'\0', b' ').decode() if m else None

def all_strings(img, minlen=4):
    """[(address, text)] -- in a .vll per section (addresses = link addresses), else file offsets"""
    out = []
    if img.elf:
        for n, x in img.secs:
            if not n or x[1] == 8 or not x[5] or n.startswith(('.rela', '.dyn', '.hash', '.shstr')): continue
            body = img.d[x[4]:x[4] + x[5]]
            base = x[3] if x[3] else x[4]
            for m in re.finditer(rb'[\x20-\x7e\t]{%d,}' % minlen, body):
                out.append((base + m.start(), n, m.group(0).decode('latin1')))
    else:
        for m in re.finditer(rb'[\x20-\x7e\t]{%d,}' % minlen, img.d):
            out.append((m.start(), '', m.group(0).decode('latin1')))
    return out

def hexdump(buf, base, labels=None, relocs=None, width=16):
    lines = []
    labels = labels or {}; relocs = relocs or {}
    for i in range(0, len(buf), width):
        a = base + i
        for k in range(a, a + width):
            if k in labels: lines.append('%08x <%s>:' % (k, labels[k]))
        chunk = buf[i:i + width]
        hx = ' '.join('%02x' % c for c in chunk)
        tx = ''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)
        notes = ['+%x -> %s' % (k - a, relocs[k]) for k in range(a, a + width) if k in relocs]
        lines.append('  %08x  %-*s  %s%s' % (a, width * 3 - 1, hx, tx, ('   ; ' + ', '.join(notes)) if notes else ''))
    return lines

def ins_comment(img, ins, func_names):
    c = []
    rl = reloc_in(img, ins)
    if ins.kind == 'call':
        nm = rl[1] if rl and rl[1] not in ('?', '') else func_names.get(ins.tg) or img.name_of(ins.tg)
        c.append('call %s' % (nm or 'sub_%x' % ins.tg))
    elif ins.op == 'lea':
        t, nm = lea_target(img, ins)
        if t is None: c.append('&%s (import)' % nm)
        else:
            lab = func_names.get(t) or nm or ''
            c.append('&0x%x %s' % (t, lab))
            s = string_at(img, t)
            if s: c.append('"%s"' % (s if len(s) < 60 else s[:57] + '...'))
    elif rl:
        c.append('reloc %d %s%+d' % rl[:3])
    return '  ; '.join(c)

def shown_text(img, ins, func_names):
    """the instruction as printed: in a .vll, bl / lea operands are the relocation targets, not the placeholder bytes"""
    rl = reloc_in(img, ins)
    if not rl: return ins.text
    if ins.kind == 'call':
        nm = rl[1] if rl[1] not in ('?', '') else None
        if nm: return 'bl     %s' % nm
        t, nm = img.reloc_value(rl)
        return 'bl     %s' % (func_names.get(t) or nm or '0x%x' % t) if t is not None else ins.text
    if ins.op == 'lea':
        t, nm = lea_target(img, ins)
        return 'lea    r%d, %s' % (ins.rd, nm if t is None else '0x%x' % t)
    return ins.text

def full_listing(img, funcs, fobjs, callers, path, cancel):
    """every byte of the code; returns statistics"""
    st = collections.Counter()
    func_names = {s: n for s, e, n, h in funcs}
    starts = sorted(func_names)
    leaders = {}
    for f in fobjs.values():
        for s in f.blocks: leaders[s] = f
    with open(path, 'w', encoding='utf-8', newline='\n') as o:
        o.write('; %s -- complete listing of %s 0x%x..0x%x (%d bytes)\n' % (
            os.path.basename(img.path), '.text' if img.elf else 'the image', img.base, img.end(), len(img.code)))
        o.write('; columns: address  bytes  instruction  ; notes.  Instructions shown as .hw / op32 / op48 are not decoded yet\n')
        o.write('; (their length is right, their meaning unknown).  "data?" = bytes no function reaches.\n\n')
        a, end = img.base, img.end()
        cur_end = img.base
        while a < end:
            if cancel and cancel(): raise ToolError('cancelled')
            if a in func_names:
                s, e, n, h = next(x for x in funcs if x[0] == a)
                cl = collections.Counter(callers.get(n, []))
                o.write('\n; ' + '=' * 100 + '\n')
                o.write('; %s   0x%x..0x%x, %d bytes   (found by: %s)\n' % (n, s, e, e - s, h))
                if cl:
                    o.write('; called from: %s%s\n' % (', '.join('%s%s' % (k, ' x%d' % v if v > 1 else '')
                                                                  for k, v in cl.most_common(12)), ' ...' if len(cl) > 12 else ''))
                o.write('; ' + '=' * 100 + '\n%s:\n' % n)
                cur_end = e
                st['functions'] += 1
            elif a in leaders and a != leaders[a].start:
                f = leaders[a]
                preds = f.pred.get(a, [])
                o.write('L_%x:%s\n' % (a, ('   ; from ' + ', '.join('L_%x' % p for p in preds[:6])) if preds else ''))
            in_func = a < cur_end
            if in_func: limit = cur_end
            else:
                k = bisect.bisect_right(starts, a)
                limit = starts[k] if k < len(starts) else end
            if limit - a < 2:
                o.write('  %08x: %-20s .byte  0x%02x\n' % (a, '%02x' % img.code[a - img.base], img.code[a - img.base]))
                st['bytes'] += 1; st['other'] += 1; a += 1
                continue
            ins = decode(img, a, limit)
            if a + ins.n > limit:                         # would run into the next function: show one halfword
                ins = Insn(a, 2, 'unk', '.hw    %04x' % img.hw(a), op='?')
            raw = img.code[a - img.base:a - img.base + min(ins.n, 6)]
            note = ins_comment(img, ins, func_names)
            if not in_func: note = ('data?  ' + note).strip()
            if ins.kind == 'switch':
                o.write('  %08x: %-20s %-40s%s\n' % (a, raw[:2].hex(' '), shown_text(img, ins, func_names), ('  ; ' + note) if note else ''))
                p = a + 2
                wide = ins.text.startswith('tbh')
                for k, t in enumerate(ins.table):
                    sz = 2 if wide else 1
                    o.write('  %08x: %-20s   case %-3d -> L_%x\n' % (p, img.code[p - img.base:p - img.base + sz].hex(' '), k, t))
                    p += sz
                if p < a + ins.n:
                    o.write('  %08x: %-20s   (table padding)\n' % (p, img.code[p - img.base:a + ins.n - img.base].hex(' ')))
            else:
                o.write('  %08x: %-20s %-40s%s\n' % (a, raw.hex(' '), shown_text(img, ins, func_names), ('  ; ' + note) if note else ''))
            st['bytes'] += ins.n
            st['insn'] += 1
            st['decoded' if ins.kind != 'unk' else 'unknown'] += 1
            a += ins.n
        o.write('\n; end of listing: %d bytes, %d instructions (%d decoded, %d not decoded yet)\n' % (
            st['bytes'], st['insn'], st['decoded'], st['unknown']))
    st['total'] = len(img.code)
    return st

def write_data(img, path):
    with open(path, 'w', encoding='utf-8', newline='\n') as o:
        if not img.elf:
            o.write('; %s is a raw image: everything is in listing.asm; strings in strings.txt\n' % os.path.basename(img.path))
            return
        labels = {v: n for n, v, sz, info, shn in img.syms if shn and n and (info & 0xf) != 2}
        relocs = {}
        for sec, off, typ, si, add in img.rel:
            if not sec.startswith('.rela.text'):
                nm = img.symname(si)
                relocs[off] = '%s%+d (type %d)' % (nm or '', add, typ) if nm else '0x%x (type %d)' % (add, typ)
        for n, x in img.secs:
            if not n or n == '.text' or n.startswith(('.rela', '.shstrtab')): continue
            o.write('\n; ---- section %s  addr 0x%x  size %d  type %d  flags 0x%x\n' % (n, x[3], x[5], x[1], x[2]))
            if x[1] == 8:
                o.write('; (no file contents: zero-filled at load)\n')
                for v in sorted(k for k in labels if x[3] <= k < x[3] + x[5]): o.write('%08x <%s>\n' % (v, labels[v]))
                continue
            body = img.d[x[4]:x[4] + x[5]]
            base = x[3] if x[3] else 0
            o.write('\n'.join(hexdump(body, base, labels if x[3] else None, relocs if x[3] else None)) + '\n')

def write_summary(img, name, funcs, st, path, sha):
    with open(path, 'w', encoding='utf-8', newline='\n') as o:
        o.write('%s\n%s\n' % (name, '=' * len(name)))
        o.write('size %d bytes, sha256 %s\n' % (len(img.d), sha))
        o.write('kind: %s\n' % ('VideoCore II ELF library (e_machine 0x%x, link address of .text 0x%x)' % (
            struct.unpack('<H', img.d[18:20])[0], img.base) if img.elf else 'raw VideoCore image (addresses = file offsets)'))
        bs = build_stamp(img.d)
        if bs: o.write('build stamp: %s\n' % bs)
        o.write('\ncode: %d bytes, %d instructions, %d decoded (%.1f %%), %d not decoded yet; %d functions\n' % (
            st['total'], st['insn'], st['decoded'], 100.0 * st['decoded'] / max(1, st['insn']), st['unknown'], len(funcs)))
        o.write('listing covers %d of %d code bytes%s\n' % (st['bytes'], st['total'], ' (all)' if st['bytes'] == st['total'] else ''))
        how = collections.Counter(h.split(' (')[0] for s, e, n, h in funcs)
        o.write('functions found by: %s\n' % ', '.join('%s %d' % (k, v) for k, v in how.items()))
        if img.elf:
            o.write('\n-- sections\n')
            for n, x in img.secs:
                if n: o.write('  %-16s addr 0x%08x  offset 0x%06x  size %7d  type %2d  flags 0x%x\n' % (n, x[3], x[4], x[5], x[1], x[2]))
            o.write('\n-- symbols (%d)\n' % len(img.syms))
            kinds = {0: 'none', 1: 'object', 2: 'func', 3: 'section', 4: 'file'}
            for i, (n, v, sz, info, shn) in enumerate(img.syms):
                if not n and not v: continue
                o.write('  %4d  0x%08x %6d  %-7s %-6s %s\n' % (i, v, sz, kinds.get(info & 0xf, str(info & 0xf)),
                                                         'import' if not shn else 'bind%d' % (info >> 4), n))
            o.write('\n-- imports (resolved by the VideoCore when the library loads)\n')
            for n in sorted(s[0] for s in img.syms if s[0] and not s[4]): o.write('  %s\n' % n)
            o.write('\n-- relocations (%d)\n' % len(img.rel))
            for sec, off, typ, si, add in img.rel:
                o.write('  %-14s 0x%08x  type %3d  %s%+d\n' % (sec, off, typ, img.symname(si), add))
        o.write('\n-- functions\n')
        for s, e, n, h in funcs:
            o.write('  0x%08x %6d  %-40s %s\n' % (s, e - s, n, h))

def disassemble_one(name, data, outdir, log, cancel, per_function=True):
    img = Image(name, data)
    funcs = discover(img)
    callers, refs, owner = xref_table(img, funcs)
    fobjs = {}
    for s, e, n, h in funcs:
        if cancel and cancel(): raise ToolError('cancelled')
        fobjs[s] = Func(img, s, e, n)
    os.makedirs(outdir, exist_ok=True)
    st = full_listing(img, funcs, fobjs, callers, os.path.join(outdir, 'listing.asm'), cancel)
    write_data(img, os.path.join(outdir, 'data.txt'))
    with open(os.path.join(outdir, 'strings.txt'), 'w', encoding='utf-8', newline='\n') as o:
        for a, sec, t in all_strings(img):
            o.write('0x%08x %-10s %s\n' % (a, sec, t))
    sha = hashlib.sha256(data).hexdigest()
    write_summary(img, name, funcs, st, os.path.join(outdir, 'summary.txt'), sha)
    fnames = []
    if per_function:
        fd = os.path.join(outdir, 'functions'); os.makedirs(fd, exist_ok=True)
        used = set()
        for s, e, n, h in funcs:
            if cancel and cancel(): raise ToolError('cancelled')
            f = fobjs[s]
            fn = safe_name(n)
            if fn in used: fn = '%s_%x' % (fn, s)
            used.add(fn)
            try:
                txt = describe(img, f, callers.get(n, []), os.path.join(fd, fn + '.dot'))
            except Exception as ex:                       # one odd function must not stop the rest
                txt = '%s: analysis failed (%s); the instructions are in listing.asm' % (n, ex)
            with open(os.path.join(fd, fn + '.txt'), 'w', encoding='utf-8', newline='\n') as o:
                o.write(txt + '\n')
            fnames.append((n, s, e - s, fn))
    edges = set()
    for t, cl in callers.items():
        for c in cl: edges.add((c, t))
    with open(os.path.join(outdir, 'callgraph.dot'), 'w', encoding='utf-8', newline='\n') as o:
        o.write('digraph calls {\n  rankdir=LR; node [shape=box fontsize=9];\n')
        for c, t in sorted(edges): o.write('  "%s" -> "%s";\n' % (c, t))
        o.write('}\n')
    log('  %-22s %6d functions, %6d instructions, %5.1f %% decoded, %d / %d code bytes listed' % (
        name, len(funcs), st['insn'], 100.0 * st['decoded'] / max(1, st['insn']), st['bytes'], st['total']))
    return {'name': name, 'sha256': sha, 'size': len(data), 'functions': len(funcs), 'insn': st['insn'],
            'decoded': st['decoded'], 'bytes': st['bytes'], 'total': st['total'], 'fnames': fnames,
            'elf': img.elf, 'stamp': build_stamp(data)}

def write_index(root, fw_name, results, files_where):
    rows = []
    for r in results:
        d = safe_name(r['name'])
        rows.append('<tr><td><b>%s</b></td><td>%s</td><td>%d</td><td>%d</td><td>%d</td><td>%.1f %%</td><td>%s</td>'
                    '<td><a href="%s/summary.txt">summary</a> &middot; <a href="%s/listing.asm">listing</a> &middot; '
                    '<a href="%s/data.txt">data</a> &middot; <a href="%s/strings.txt">strings</a></td></tr>' % (
                        html.escape(r['name']), 'ELF library' if r['elf'] else 'raw image', r['size'], r['functions'],
                        r['insn'], 100.0 * r['decoded'] / max(1, r['insn']), html.escape(r['stamp'] or ''), d, d, d, d))
    fsec = []
    for r in results:
        d = safe_name(r['name'])
        links = ' '.join('<a href="%s/functions/%s.txt">%s</a>' % (d, html.escape(fn), html.escape(n)) for n, s, sz, fn in r['fnames'])
        fsec.append('<h2 id="%s">%s <small>(%d functions)</small></h2><p class="fl">%s</p>' % (d, html.escape(r['name']), len(r['fnames']), links))
    page = '''<!doctype html><html><head><meta charset="utf-8"><title>VideoCore disassembly: %s</title>
<style>body{font-family:system-ui,Segoe UI,Arial,sans-serif;margin:24px;max-width:1200px;color:#1d1d1f;background:#fff}
table{border-collapse:collapse;width:100%%;font-size:14px}td,th{border:1px solid #ddd;padding:4px 8px;text-align:left}
th{background:#f0f3f7}a{color:#1f5f8b;text-decoration:none}.fl a{display:inline-block;margin:0 10px 4px 0;font-family:
Consolas,monospace;font-size:12px}small{color:#666;font-weight:normal}.note{background:#f3f7fa;border-left:3px solid
#1f5f8b;padding:8px 12px}</style></head><body>
<h1>VideoCore disassembly &mdash; %s</h1>
<p class="note">Made by iPod VideoCore Extractor &amp; Disassembler %s on %s. Every code byte is listed; instructions not
decoded yet are shown with their exact encoding (.hw / op32 / op48). The files themselves were put in: %s</p>
<table><tr><th>File</th><th>Kind</th><th>Bytes</th><th>Functions</th><th>Instructions</th><th>Decoded</th><th>Build</th><th></th></tr>
%s</table>%s</body></html>''' % (html.escape(fw_name), html.escape(fw_name), TOOL_VERSION, time.strftime('%Y-%m-%d %H:%M'),
                                html.escape(files_where or '(not extracted)'), '\n'.join(rows), '\n'.join(fsec))
    with open(os.path.join(root, 'index.html'), 'w', encoding='utf-8') as o:
        o.write(page)

README = '''VideoCore disassembly of %(fw)s
%(line)s

One folder per file.  Start with index.html (open it in a browser) or a file's summary.txt.

  summary.txt      size, SHA-256, sections, every symbol, imports, every relocation, decode coverage, functions
  listing.asm      the whole code section, every byte: function headers (with callers), block labels (with the
                   blocks that lead there), call / branch targets by name, relocations, the strings lea points at
  data.txt         the data sections as hex + text; data symbols and relocated pointers marked
  strings.txt      every printable string (4+ characters) with its address
  functions/*.txt  per function: blocks and CFG edges, callers / callees, register notes, memory accesses,
                   reconstructed structures (fields reached through argument registers), goto-level pseudocode
  functions/*.dot  the function's control-flow graph (Graphviz: dot -Tsvg f.dot -o f.svg)
  callgraph.dot    the call graph of the file

Addresses: in a .vll, link addresses (.text starts at 0x100000; the VideoCore relocates the library when it loads
it).  In vmcs.bin / RenderServer.bin, file offsets (vmcs.bin is linked at 0).

The instruction set.  VideoCore II (BCM2722) is not documented publicly.  Instruction LENGTHS are known for all
instructions (top byte 0x2f = 48 bit; 0x10-0x1f with bit 7, 0x1e / 0x1f and 0x28-0x2e = 32 bit; the rest 16 bit),
so the listing stays aligned.  Decoded so far: bl, lea (pc-relative / absolute), mov #imm16 / #imm32 / #imm4, mov,
add, add #imm, rsub, lsr #imm, ld / st [rS + 4k], ld / st [rS], the 0x2c load and the 0x28 access (widths unsure),
conditional branches (0x0CIR + 0xFFoo), b (8-bit and 23-bit), tbb / tbh switch tables, push / pop, rts, and
add r(16+N), #imm16 (r25 = stack pointer).  Everything else is printed as .hw / op32 / op48 with its encoding.
In a .vll, call and lea targets come from the relocations, not from the (unrelocated) instruction bytes.

Decode coverage per file (instructions with a mnemonic):
%(cov)s

Made by iPod VideoCore Extractor & Disassembler %(ver)s (bcm-rockbox, tools/bcm/ipod_vc_disasm.pyw).
'''

def run_job(inp, files_out, dis_out, rockbox, log, progress, cancel, per_function=True, do_extract=True, do_dis=True):
    """the whole job; returns a short result text"""
    t0 = time.time()
    if not inp or not os.path.exists(inp): raise ToolError('choose the .ipsw (or Firmware file / folder) first')
    sets = []                                                  # (firmware name, {path: bytes})
    if os.path.isdir(inp):
        files = videocore_from_folder(inp)
        if not files: raise ToolError('no VideoCore files (.vll / vmcs.bin) in %s' % inp)
        sets.append((os.path.basename(os.path.normpath(inp)) or 'folder', files))
    else:
        log('reading %s ...' % os.path.basename(inp))
        for fw, img in firmware_images(inp):
            files = extract_videocore(img)
            log('%s: %d bytes, %s' % (fw, len(img), '%d VideoCore files' % len(files) if files else 'no VideoCore files'))
            if files: sets.append((fw, files))
        if not sets:
            raise ToolError('no VideoCore files found. This tool is for the iPod 5th / 5.5 generation ("iPod Video") '
                            'firmware; other iPods have no VideoCore.')
    total = sum(len(f) for n, f in sets) * (2 if do_dis else 1) or 1
    done = 0
    summary = []
    for fw, files in sets:
        if cancel(): raise ToolError('cancelled')
        files_where = None
        if do_extract and files_out:
            if rockbox:
                dest = os.path.join(files_out, '.rockbox', 'VideoCore')
                os.makedirs(dest, exist_ok=True)
                for p, d in sorted(files.items()):
                    with open(os.path.join(dest, os.path.basename(p)), 'wb') as o: o.write(d)
                    done += 1; progress(done / total)
                files_where = dest
                log('files -> %s (flat, where the bcm-rockbox video player reads them)' % dest)
            else:
                dest = os.path.join(files_out, 'iPod VideoCore files', safe_name(fw))
                for p, d in sorted(files.items()):
                    q = os.path.join(dest, *[safe_name(x) for x in p.strip('/').split('/')])
                    os.makedirs(os.path.dirname(q), exist_ok=True)
                    with open(q, 'wb') as o: o.write(d)
                    done += 1; progress(done / total)
                with open(os.path.join(dest, 'SHA256SUMS.txt'), 'w', newline='\n') as o:
                    for p, d in sorted(files.items()): o.write('%s  %s\n' % (hashlib.sha256(d).hexdigest(), p.strip('/')))
                files_where = dest
                log('files -> %s' % dest)
            for p, d in sorted(files.items()):
                log('    %-46s %8d bytes' % (p, len(d)))
        if do_dis and dis_out:
            root = os.path.join(dis_out, 'iPod VideoCore disassembly', safe_name(fw))
            os.makedirs(root, exist_ok=True)
            log('disassembling into %s' % root)
            results = []
            for p, d in sorted(files.items(), key=lambda kv: len(kv[1])):
                if cancel(): raise ToolError('cancelled')
                nm = os.path.basename(p)
                if len(d) < 64:
                    log('  %-22s skipped (%d bytes)' % (nm, len(d))); done += 1; continue
                results.append(disassemble_one(nm, d, os.path.join(root, safe_name(nm)), log, cancel, per_function))
                done += 1; progress(done / total)
            results.sort(key=lambda r: r['name'].lower())
            write_index(root, fw, results, files_where)
            cov = '\n'.join('  %-22s %6d of %6d instructions (%.1f %%), %d / %d code bytes listed' % (
                r['name'], r['decoded'], r['insn'], 100.0 * r['decoded'] / max(1, r['insn']), r['bytes'], r['total']) for r in results)
            with open(os.path.join(root, 'README.txt'), 'w', encoding='utf-8', newline='\n') as o:
                o.write(README % {'fw': fw, 'line': '=' * (31 + len(fw)), 'cov': cov, 'ver': TOOL_VERSION})
            with open(os.path.join(root, 'manifest.json'), 'w') as o:
                json.dump({'firmware': fw, 'files': {r['name']: r['sha256'] for r in results}}, o, indent=1)
            summary.append('%s: %d files disassembled -> %s' % (fw, len(results), root))
        elif files_where:
            summary.append('%s: %d files -> %s' % (fw, len(files), files_where))
    progress(1.0)
    log('done in %.1f s' % (time.time() - t0))
    return '\n'.join(summary)

# =====================================================================================================================
#  drives
# =====================================================================================================================
def list_drives():
    """[(label, path)] -- drive letters on Windows, mounted volumes elsewhere; Rockbox iPods marked"""
    out = []
    if os.name == 'nt':
        for c in string.ascii_uppercase:
            p = '%s:\\' % c
            if os.path.exists(p):
                tag = '  (iPod with Rockbox)' if os.path.isdir(os.path.join(p, '.rockbox')) else ''
                out.append(('%s%s' % (p, tag), p))
    else:
        cands = ['/Volumes'] if sys.platform == 'darwin' else ['/media', '/run/media', '/mnt']
        seen = set()
        for base in cands:
            try:
                for d in sorted(os.listdir(base)):
                    p = os.path.join(base, d)
                    if not os.path.isdir(p): continue
                    subs = [p]
                    if base != '/Volumes':
                        try: subs += [os.path.join(p, x) for x in sorted(os.listdir(p)) if os.path.isdir(os.path.join(p, x))]
                        except OSError: pass
                    for q in subs:
                        if q in seen or not os.path.ismount(q) and q != p: continue
                        seen.add(q)
                        tag = '  (iPod with Rockbox)' if os.path.isdir(os.path.join(q, '.rockbox')) else ''
                        out.append(('%s%s' % (q, tag), q))
            except OSError:
                pass
        home = os.path.expanduser('~')
        out.append((home + '  (home folder)', home))
    return out

def is_rockbox_drive(p):
    return bool(p) and os.path.isdir(os.path.join(p, '.rockbox'))

# =====================================================================================================================
#  window
# =====================================================================================================================
def gui(prefill=None):
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    import queue

    root = tk.Tk()
    root.title('iPod VideoCore Extractor & Disassembler')
    root.minsize(760, 560)
    try: ttk.Style().theme_use('vista' if os.name == 'nt' else 'clam')
    except tk.TclError: pass
    q = queue.Queue()
    state = {'running': False, 'cancel': False}

    v_in = tk.StringVar(); v_files = tk.StringVar(); v_dis = tk.StringVar()
    v_extract = tk.BooleanVar(value=True); v_dis_on = tk.BooleanVar(value=True)
    v_rb = tk.BooleanVar(value=False); v_perfn = tk.BooleanVar(value=True)
    v_status = tk.StringVar(value='Choose the firmware, then where the files and the disassembly go.')
    if prefill: v_in.set(prefill)                              # a file dropped on the tool's icon

    pad = {'padx': 8, 'pady': 4}
    frm = ttk.Frame(root, padding=12); frm.pack(fill='both', expand=True)
    frm.columnconfigure(1, weight=1)

    ttk.Label(frm, text='iPod VideoCore Extractor & Disassembler', font=('TkDefaultFont', 13, 'bold')).grid(
        row=0, column=0, columnspan=3, sticky='w', pady=(0, 2))
    ttk.Label(frm, text='For the iPod 5th / 5.5 generation (iPod Video) firmware: .ipsw, Firmware-* file, or a folder of VideoCore files.',
              foreground='#555').grid(row=1, column=0, columnspan=3, sticky='w', pady=(0, 10))

    def pick_in():
        p = filedialog.askopenfilename(title='iPod firmware (.ipsw or Firmware-*)',
                                       filetypes=[('iPod firmware', '*.ipsw'), ('All files', '*.*')])
        if p: v_in.set(p)
    def pick_in_dir():
        p = filedialog.askdirectory(title='Folder that holds VideoCore files')
        if p: v_in.set(p)
    ttk.Label(frm, text='1. Firmware').grid(row=2, column=0, sticky='w', **pad)
    ttk.Entry(frm, textvariable=v_in).grid(row=2, column=1, sticky='ew', **pad)
    bf = ttk.Frame(frm); bf.grid(row=2, column=2, sticky='w')
    ttk.Button(bf, text='File...', command=pick_in).pack(side='left')
    ttk.Button(bf, text='Folder...', command=pick_in_dir).pack(side='left', padx=(4, 0))

    drives = list_drives()
    def drive_picker(row, label, var, on_change=None):
        ttk.Label(frm, text=label).grid(row=row, column=0, sticky='w', **pad)
        cb = ttk.Combobox(frm, textvariable=var, values=[d[0] for d in drives])
        cb.grid(row=row, column=1, sticky='ew', **pad)
        def chosen(_e=None):
            for lab, p in drives:
                if var.get() == lab: var.set(p)
            if on_change: on_change()
        cb.bind('<<ComboboxSelected>>', chosen)
        def browse():
            p = filedialog.askdirectory(title=label)
            if p: var.set(p); on_change and on_change()
        f2 = ttk.Frame(frm); f2.grid(row=row, column=2, sticky='w')
        ttk.Button(f2, text='Browse...', command=browse).pack(side='left')
        def refresh():
            drives[:] = list_drives(); cb['values'] = [d[0] for d in drives]
        ttk.Button(f2, text='\u21bb', width=3, command=refresh).pack(side='left', padx=(4, 0))
        return cb

    def files_changed():
        rb = is_rockbox_drive(v_files.get())
        rb_chk.state(['!disabled'] if rb else ['disabled'])
        if rb and not v_rb.get(): v_rb.set(True)
        if not rb: v_rb.set(False)
    drive_picker(3, '2. Put the files on', v_files, files_changed)
    opt1 = ttk.Frame(frm); opt1.grid(row=4, column=1, columnspan=2, sticky='w', padx=8)
    ttk.Checkbutton(opt1, text='Extract the VideoCore files', variable=v_extract).pack(side='left')
    rb_chk = ttk.Checkbutton(opt1, text='Rockbox iPod: put them in /.rockbox/VideoCore (for the video player)', variable=v_rb)
    rb_chk.pack(side='left', padx=(16, 0)); rb_chk.state(['disabled'])

    drive_picker(5, '3. Put the disassembly in', v_dis)
    opt2 = ttk.Frame(frm); opt2.grid(row=6, column=1, columnspan=2, sticky='w', padx=8)
    ttk.Checkbutton(opt2, text='Disassemble', variable=v_dis_on).pack(side='left')
    ttk.Checkbutton(opt2, text='One analysis file per function (CFG, xrefs, structures, pseudocode)', variable=v_perfn).pack(side='left', padx=(16, 0))

    pb = ttk.Progressbar(frm, mode='determinate', maximum=1000)
    pb.grid(row=7, column=0, columnspan=3, sticky='ew', padx=8, pady=(12, 4))
    ttk.Label(frm, textvariable=v_status, foreground='#333').grid(row=8, column=0, columnspan=3, sticky='w', padx=8)

    lf = ttk.Frame(frm); lf.grid(row=9, column=0, columnspan=3, sticky='nsew', padx=8, pady=6)
    frm.rowconfigure(9, weight=1)
    txt = tk.Text(lf, height=14, wrap='none', font=('Consolas' if os.name == 'nt' else 'TkFixedFont', 9),
                  background='#fbfbfc', relief='solid', borderwidth=1)
    sb = ttk.Scrollbar(lf, command=txt.yview); txt.configure(yscrollcommand=sb.set)
    txt.pack(side='left', fill='both', expand=True); sb.pack(side='right', fill='y')

    bar = ttk.Frame(frm); bar.grid(row=10, column=0, columnspan=3, sticky='e', padx=8, pady=(4, 0))
    def open_out():
        p = os.path.join(v_dis.get(), 'iPod VideoCore disassembly') if v_dis.get() else ''
        if p and os.path.isdir(p):
            if os.name == 'nt': os.startfile(p)
            elif sys.platform == 'darwin': os.system('open "%s"' % p)
            else: os.system('xdg-open "%s" >/dev/null 2>&1 &' % p)
    b_open = ttk.Button(bar, text='Open disassembly folder', command=open_out, state='disabled'); b_open.pack(side='left')
    b_go = ttk.Button(bar, text='Start'); b_go.pack(side='left', padx=(8, 0))

    def log(s): q.put(('log', s))
    def progress(f): q.put(('prog', f))
    def worker(args):
        try:
            res = run_job(*args, log=log, progress=progress, cancel=lambda: state['cancel'],
                          per_function=v_perfn.get(), do_extract=v_extract.get(), do_dis=v_dis_on.get())
            q.put(('done', res))
        except ToolError as e:
            q.put(('err', str(e)))
        except Exception as e:                                  # a full report for anything unexpected
            import traceback
            q.put(('err', 'unexpected error: %s\n%s' % (e, traceback.format_exc())))

    def start():
        if state['running']:
            state['cancel'] = True; v_status.set('Stopping...'); return
        inp, fo, do = v_in.get().strip(), v_files.get().strip(), v_dis.get().strip()
        if not inp: messagebox.showwarning('Firmware', 'Choose the .ipsw, a Firmware-* file or a folder first.'); return
        if v_extract.get() and not fo:
            messagebox.showwarning('Files', 'Choose where the extracted files go (or untick "Extract").'); return
        if v_dis_on.get() and not do:
            messagebox.showwarning('Disassembly', 'Choose where the disassembly goes (or untick "Disassemble").'); return
        if not v_extract.get() and not v_dis_on.get(): return
        for p in (fo if v_extract.get() else None, do if v_dis_on.get() else None):
            if p and not os.path.isdir(p):
                messagebox.showerror('Folder', '%s does not exist.' % p); return
        txt.delete('1.0', 'end'); pb['value'] = 0
        state.update(running=True, cancel=False)
        b_go.configure(text='Stop'); b_open.state(['disabled']); v_status.set('Working...')
        threading.Thread(target=worker, args=((inp, fo, do, v_rb.get() and is_rockbox_drive(fo)),), daemon=True).start()
    b_go.configure(command=start)

    def poll():
        try:
            while True:
                k, v = q.get_nowait()
                if k == 'log': txt.insert('end', v + '\n'); txt.see('end')
                elif k == 'prog': pb['value'] = int(v * 1000)
                else:
                    state['running'] = False; b_go.configure(text='Start')
                    if k == 'done':
                        pb['value'] = 1000
                        v_status.set('Finished.'); txt.insert('end', '\n' + v + '\n'); txt.see('end')
                        if v_dis_on.get(): b_open.state(['!disabled'])
                    else:
                        v_status.set('Stopped.' if v == 'cancelled' else 'Failed -- see the log.')
                        txt.insert('end', '\n' + v + '\n'); txt.see('end')
        except queue.Empty:
            pass
        root.after(100, poll)
    root.after(100, poll)
    root.mainloop()

# =====================================================================================================================
def main(argv):
    if '--input' in argv or '-h' in argv or '--help' in argv:
        if '-h' in argv or '--help' in argv:
            print(__doc__); return 0
        def arg(k):
            return argv[argv.index(k) + 1] if k in argv and argv.index(k) + 1 < len(argv) else None
        inp, fo, do = arg('--input'), arg('--files-out'), arg('--dis-out')
        try:
            res = run_job(inp, fo, do, '--rockbox' in argv and is_rockbox_drive(fo), log=print,
                          progress=lambda f: None, cancel=lambda: False, per_function='--no-functions' not in argv,
                          do_extract=bool(fo), do_dis=bool(do))
        except ToolError as e:
            print('error:', e); return 1
        print(res)
        return 0
    try:
        gui(argv[1] if len(argv) > 1 and os.path.exists(argv[1]) else None)
    except ImportError:
        print('Tkinter is not installed: use the command line (--help)'); return 1
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))

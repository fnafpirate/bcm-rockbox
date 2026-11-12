#!/usr/bin/env python3
"""vc2wb.py -- a VideoCore II (BCM2722) analysis workbench.  Standard-library Python only.

Builds on vc2dis.py (the instruction decoder used since v12) and vc_elf_callgraph.py.  Works on Apple's .vll libraries
(ELF, e_machine 0x5f, symbols + relocations) and on raw images such as vmcs.bin (linked at 0).

  vc2wb.py info   FILE                      sections, symbols, relocations / image size, build stamp
  vc2wb.py funcs  FILE                      function discovery: symbols, call targets, prologue scan
  vc2wb.py func   FILE NAME|ADDR [--dot F]  one function: blocks, CFG, xrefs, registers, memory, structures, pseudocode
  vc2wb.py xrefs  FILE NAME|ADDR            who calls / references it, what it calls
  vc2wb.py report FILE OUTDIR               all of the above for every function + index.txt + callgraph.dot
  --strict                                  an unknown instruction forgets every register (default: the two low nibbles)

What each part can and cannot do (honest limits):
  * The decoder knows the encodings listed in DECODED below.  Everything else is shown as .hw / op32 / op48 and treated as
    "unknown": register tracking forgets the registers named by its two low nibbles (the destination / source fields of
    every decoded form); --strict forgets all of them (nothing can then be carried past an unknown instruction).
  * In a .vll the operands of lea / bl come from the relocations (symbol + addend), not from the unrelocated bytes.
  * Function discovery on ELF = the symbol table (exact) + bl targets + relocation targets.  On a raw image = bl targets
    that land on a push, plus pushes right after a return; extents run to the next start (may include padding / data).
  * CFG: blocks split at every branch target and after every branch / return; tbb / tbh tables are read up to the
    lowest target (the usual layout); a table guarded differently may be cut short (shown as "table?").
  * Register tracking: one forward pass over each block in address order with the entry state merged from the
    predecessors (values that agree survive).  Values: constants, lea addresses, ARGn (r0..r5 at entry) + offset, and one
    level of loaded fields (ARGn->f_XX).
  * Structures: fields = offsets read / written through ARGn and ARGn->f_XX pointers, with access widths (u32 unless the
    instruction says otherwise -- most 16-bit forms are word accesses; 0x28 / 0x2c are listed as "16/32?").
  * Pseudocode: goto-level C, one statement per decoded instruction; calls show the r0..r5 values known at the call.

DECODED (statistics in vc2dis.py's header):
  bl, lea (pc-relative / absolute), mov #imm32, conditional branch 0x0CIR+0xFFoo, b (8-bit and 23-bit), tbb / tbh,
  push / pop, rts, ld / st [rS+4k], ld / st [rS], add, rsub, add #i, lsr #i, mov, mov #i, mov #imm16, 0x2c load, 0x28,
  and (v96, from the prologue / epilogue pairs) 0x2e5N imm16 = add r(16+N), #imm -- r25 is the stack pointer.
"""
import sys, os, struct, bisect, re, collections

# ------------------------------------------------------------------------------------------------------------ files
def sx(v, b):
    return v - (1 << b) if v & (1 << (b - 1)) else v

class Image:
    """an ELF .vll or a raw image; .code = bytes of the executable part, .base = its address"""
    def __init__(self, path):
        self.path = path
        self.d = open(path, 'rb').read()
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
            L.append('  %06x: %-18s %-42s %s%s' % (i.a, raw, i.text, n, r))
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

def main(argv):
    if len(argv) < 3:
        print(__doc__); return 1
    cmd, path = argv[1], argv[2]
    if '--strict' in argv:
        Func.strict = True; argv = [x for x in argv if x != '--strict']
    img = Image(path)
    funcs = discover(img)
    if cmd == 'info':
        print('%s: %s, %d bytes, code 0x%x..0x%x' % (path, 'ELF (.vll)' if img.elf else 'raw image', len(img.d), img.base, img.end()))
        for n, x in img.secs:
            if n: print('  section %-14s addr 0x%07x size %6d' % (n, x[3], x[5]))
        if img.elf:
            fn = sum(1 for s in img.syms if s[3] & 0xf == 2 and s[4]); und = sum(1 for s in img.syms if s[0] and not s[4])
            print('  %d defined functions, %d imported symbols, %d relocations' % (fn, und, len(img.rel)))
        m = re.search(rb'(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-9]\d \d{4}\x00\d\d:\d\d:\d\d', img.d)
        if m: print('  build stamp: %s' % m.group(0).replace(b'\0', b' ').decode())
        print('  %d functions found' % len(funcs))
    elif cmd == 'funcs':
        how = collections.Counter(h.split(' (')[0] for s, e, n, h in funcs)
        print('# %d functions: %s' % (len(funcs), ', '.join('%d %s' % (v, k) for k, v in how.items())))
        for s, e, n, h in funcs:
            f = Func(img, s, e, n)
            print('0x%06x %6d  %3d blocks  %-36s %s' % (s, e - s, len(f.blocks), n, h))
    elif cmd == 'func':
        s, e, n = find(img, funcs, argv[3])
        dot = argv[argv.index('--dot') + 1] if '--dot' in argv else None
        callers, refs, owner = xref_table(img, funcs)
        print(describe(img, Func(img, s, e, n), callers.get(n, []), dot))
    elif cmd == 'xrefs':
        s, e, n = find(img, funcs, argv[3])
        callers, refs, owner = xref_table(img, funcs)
        print('%s @0x%x' % (n, s))
        print('  called by: %s' % ', '.join('%s (%d)' % kv for kv in collections.Counter(callers.get(n, [])).most_common()))
        print('  address taken in: %s' % ', '.join(sorted(set(refs.get(s, [])))))
        f = Func(img, s, e, n)
        cs = []
        for i in f.ins:
            if i.kind == 'call':
                rl = reloc_in(img, i)
                cs.append(rl[1] if rl and rl[1] != '?' else img.name_of(i.tg) or 'sub_%x' % i.tg)
        print('  calls: %s' % ', '.join('%s (%d)' % kv for kv in collections.Counter(cs).most_common()))
    elif cmd == 'report':
        out = argv[3]
        os.makedirs(out, exist_ok=True)
        callers, refs, owner = xref_table(img, funcs)
        idx, edges = [], set()
        totals = collections.Counter()
        for s, e, n, h in funcs:
            f = Func(img, s, e, n)
            txt = describe(img, f, callers.get(n, []))
            safe = re.sub(r'[^A-Za-z0-9_.]', '_', n)
            open(os.path.join(out, '%s.txt' % safe), 'w').write(txt + '\n')
            unk = sum(1 for i in f.ins if i.kind == 'unk')
            totals['insn'] += len(f.ins); totals['unk'] += unk; totals['blocks'] += len(f.blocks)
            idx.append('0x%06x %6d  %3d blocks  %4d/%-4d decoded  %-36s %s' % (s, e - s, len(f.blocks), len(f.ins) - unk,
                                                                          len(f.ins), n, h))
        for t, cl in callers.items():
            for c in cl: edges.add((c, t))
        with open(os.path.join(out, 'index.txt'), 'w') as o:
            o.write('%s: %d functions, %d instructions (%d decoded, %.1f %%), %d basic blocks\n\n' % (
                path, len(funcs), totals['insn'], totals['insn'] - totals['unk'],
                100.0 * (totals['insn'] - totals['unk']) / max(1, totals['insn']), totals['blocks']))
            o.write('\n'.join(idx) + '\n')
        with open(os.path.join(out, 'callgraph.dot'), 'w') as o:
            o.write('digraph calls {\n  rankdir=LR; node [shape=box fontsize=9];\n')
            for c, t in sorted(edges): o.write('  "%s" -> "%s";\n' % (c, t))
            o.write('}\n')
        print('%d functions -> %s (index.txt, callgraph.dot, one .txt per function)' % (len(funcs), out))
    else:
        print(__doc__); return 1
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""iPod Defrag 1.1 (bcm overlay v144) -- finds and fixes fragmented files on an exFAT iPod.

Windows' own Defragment tool does not do exFAT.  This reads the iPod's exFAT file system directly, lists every file and
folder that is in more than one piece, and moves each one into a single run of free clusters.  If no free run is big
enough, files are first packed toward the start of the disk (lowest free space first) until one is.

Safety: every move copies the data to free space first, reads the copy back and compares it, and only then changes the
file's directory entry; the old clusters are released last.  If the computer or the cable fails in the middle, the file
is either still at its old place or already at its new one -- at worst some clusters stay marked as used (Windows'
"Check" / chkdsk gives them back).  The volume is marked dirty while it works and clean again at the end.

Windows: double-click (it asks for administrator rights: raw access to the drive).  Close Explorer windows and iTunes
on the iPod first; the drive is locked while the tool works.
Command line:
    ipod_defrag.pyw --scan E:          (Windows drive letter)      list what is fragmented
    ipod_defrag.pyw --defrag E: --yes                               fix it
    ipod_defrag.pyw --scan /dev/sdb2   (Linux / macOS: the unmounted partition, or an image file)
    ipod_defrag.pyw --check E: [--fix --yes]                       check the volume (and repair it)
Options: --no-verify (skip reading every copy back), --no-pack (never move unfragmented files),
         --files-only (leave folders alone), --full (pack every file to the start: free space in one piece)

1.1: "Check & fix".  A volume marked dirty (the iPod lost power, crashed or was unplugged while writing) could only be
defragmented after Windows' "Check" / chkdsk.  The check here goes through every file and folder: clusters marked used
that no file owns (what an interrupted write leaves) are given back, clusters a file owns that are marked free are
marked used, and the dirty mark is cleared.  It repairs nothing else: a damaged directory entry, a cluster owned by two
files or a broken cluster chain stop it (then chkdsk /f is the right tool).
"""
import os, sys, struct, time, bisect, threading

VERSION = '1.1'
APP = 'iPod Defrag'

def log_path():
    base = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, 'frozen', False) else sys.argv[0]))
    return os.path.join(base, 'ipod_defrag_log.txt')

class DefragError(Exception):
    pass

def human(n):
    for u in ('B', 'KB', 'MB', 'GB', 'TB'):
        if n < 1024 or u == 'TB':
            return ('%d %s' % (n, u)) if u == 'B' else ('%.1f %s' % (n, u))
        n /= 1024.0

# --------------------------------------------------------------------------------------------------------------------
# raw access: an image / device file, or a Windows volume (\\.\E:)

class FileDev:
    def __init__(self, path, writable):
        self.f = open(path, 'r+b' if writable else 'rb', buffering=0)
        self.ss = 512
        self.name = path
    def read(self, off, n):
        self.f.seek(off)
        out = b''
        while len(out) < n:
            d = self.f.read(n - len(out))
            if not d:
                raise DefragError('read past the end at %d' % (off + len(out)))
            out += d
        return out
    def write(self, off, data):
        self.f.seek(off)
        self.f.write(data)
    def flush(self):
        try:
            self.f.flush(); os.fsync(self.f.fileno())
        except OSError:
            pass
    def lock(self):
        pass
    def close(self):
        try:
            self.flush()
        finally:
            self.f.close()

if os.name == 'nt':
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
    FSCTL_LOCK_VOLUME, FSCTL_UNLOCK_VOLUME, FSCTL_DISMOUNT_VOLUME = 0x00090018, 0x0009001C, 0x00090020
    INVALID_HANDLE = ctypes.c_void_p(-1).value
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                    ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                             ctypes.c_void_p]
    k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                              ctypes.c_void_p]
    k32.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong, ctypes.c_void_p, wintypes.DWORD]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
    k32.GetVolumeInformationW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD, ctypes.c_void_p,
                                          ctypes.c_void_p, ctypes.c_void_p, wintypes.LPWSTR, wintypes.DWORD]
    k32.GetLogicalDrives.restype = wintypes.DWORD
    k32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]

    def is_admin():
        try:
            return bool(ctypes.windll.shell32.IsUserAnAdmin())
        except Exception:
            return False

    class WinVolume:
        """\\\\.\\E: -- offsets and lengths are kept to whole sectors (raw volume access needs that)"""
        def __init__(self, letter, writable):
            self.name = letter.rstrip('\\/').upper()
            if len(self.name) == 1:
                self.name += ':'
            path = '\\\\.\\' + self.name
            access = GENERIC_READ | (GENERIC_WRITE if writable else 0)
            self.h = k32.CreateFileW(path, access, 3, None, 3, 0, None)
            if self.h in (None, INVALID_HANDLE):
                raise DefragError('cannot open %s (error %d)%s' % (self.name, ctypes.get_last_error(),
                                  '' if is_admin() else ' -- run as administrator'))
            self.ss = 512
            self.locked = False
        def _io(self, off, buf, n, write):
            if not k32.SetFilePointerEx(self.h, off, None, 0):
                raise DefragError('seek to %d failed (error %d)' % (off, ctypes.get_last_error()))
            done = wintypes.DWORD(0)
            ok = (k32.WriteFile if write else k32.ReadFile)(self.h, buf, n, ctypes.byref(done), None)
            if not ok or done.value != n:
                raise DefragError('%s of %d bytes at %d failed (error %d)' % ('write' if write else 'read', n, off,
                                  ctypes.get_last_error()))
        def read(self, off, n):
            ss = self.ss
            a = off - off % ss
            e = -(-(off + n) // ss) * ss
            out = bytearray()
            pos = a
            while pos < e:
                k = min(e - pos, 8 << 20)
                b = ctypes.create_string_buffer(k)
                self._io(pos, b, k, False)
                out += b.raw
                pos += k
            return bytes(out[off - a: off - a + n])
        def write(self, off, data):
            ss = self.ss
            a = off - off % ss
            e = -(-(off + len(data)) // ss) * ss
            if a != off or e != off + len(data):
                whole = bytearray(self.read(a, e - a))
                whole[off - a: off - a + len(data)] = data
                data = bytes(whole); off = a
            pos = 0
            while pos < len(data):
                k = min(len(data) - pos, 8 << 20)
                b = ctypes.create_string_buffer(data[pos:pos + k], k)
                self._io(off + pos, b, k, True)
                pos += k
        def flush(self):
            k32.FlushFileBuffers(self.h)
        def _ioctl(self, code):
            r = wintypes.DWORD(0)
            return k32.DeviceIoControl(self.h, code, None, 0, None, 0, ctypes.byref(r), None)
        def lock(self):
            for _ in range(10):
                if self._ioctl(FSCTL_LOCK_VOLUME):
                    self._ioctl(FSCTL_DISMOUNT_VOLUME)
                    self.locked = True
                    return
                time.sleep(0.5)
            raise DefragError('%s is in use -- close Explorer windows, iTunes and anything else that has files '
                              'open on the iPod, then try again' % self.name)
        def close(self):
            try:
                self.flush()
                if self.locked:
                    self._ioctl(FSCTL_UNLOCK_VOLUME)
            finally:
                k32.CloseHandle(self.h)

    def exfat_drives():
        """[(letter, label, is_ipod)] of the exFAT volumes Windows sees"""
        out = []
        mask = k32.GetLogicalDrives()
        for i in range(26):
            if not mask & (1 << i):
                continue
            root = '%s:\\' % chr(65 + i)
            fs = ctypes.create_unicode_buffer(64); lab = ctypes.create_unicode_buffer(256)
            if not k32.GetVolumeInformationW(root, lab, 256, None, None, None, fs, 64):
                continue
            if fs.value.upper() != 'EXFAT':
                continue
            ipod = any(os.path.isdir(root + d) for d in ('iPod_Control', '.rockbox'))
            out.append((root[:2], lab.value, ipod))
        return out
else:
    def is_admin():
        return os.geteuid() == 0 if hasattr(os, 'geteuid') else True
    def exfat_drives():
        return []

def open_dev(target, writable):
    if os.name == 'nt' and len(target.rstrip(':\\/')) == 1 and target[0].isalpha():
        return WinVolume(target, writable)
    return FileDev(target, writable)

# --------------------------------------------------------------------------------------------------------------------
# exFAT

def le16(b, o): return struct.unpack_from('<H', b, o)[0]
def le32(b, o): return struct.unpack_from('<I', b, o)[0]
def le64(b, o): return struct.unpack_from('<Q', b, o)[0]

def set_checksum(ents):
    c = 0
    for i, x in enumerate(ents):
        if i in (2, 3):
            continue
        c = (((c & 1) << 15) | (c >> 1)) + x
        c &= 0xffff
    return c

def runs_of(clusters):
    """[(start, length)] of a cluster list"""
    out = []
    for c in clusters:
        if out and out[-1][0] + out[-1][1] == c:
            out[-1][1] += 1
        else:
            out.append([c, 1])
    return [tuple(r) for r in out]

class Item:
    """a file or folder: where its stream extension entry is, and its clusters"""
    __slots__ = ('path', 'is_dir', 'parent', 'set_off', 'set_len', 'first', 'nofat', 'size', 'clusters', 'dirobj')
    def __init__(self):
        self.dirobj = None

class DirContent:
    """a folder's data: its clusters (kept current when the folder itself is moved)"""
    def __init__(self, clusters):
        self.clusters = clusters

class Volume:
    def __init__(self, dev, log=print):
        self.dev, self.log = dev, log
        vbr = dev.read(0, 512)
        if vbr[3:11] != b'EXFAT   ':
            if vbr[82:90] == b'FAT32   ':
                raise DefragError('this is FAT32, not exFAT -- Windows\' own Defragment tool handles FAT32')
            raise DefragError('not an exFAT volume')
        self.bps = 1 << vbr[0x6C]
        dev.ss = self.bps
        self.spc = 1 << vbr[0x6D]
        self.cs = self.bps * self.spc
        self.fat_off = le32(vbr, 0x50) * self.bps
        self.fat_len = le32(vbr, 0x54) * self.bps
        self.heap = le32(vbr, 0x58) * self.bps
        self.nclus = le32(vbr, 0x5C)
        self.root = le32(vbr, 0x60)
        self.flags = le16(vbr, 0x6A)
        if vbr[0x6E] != 1:
            raise DefragError('this exFAT volume has %d FATs (TexFAT) -- not supported' % vbr[0x6E])
        self.dirty = bool(self.flags & 2)
        fatbytes = (self.nclus + 2) * 4
        self.fat = bytearray(dev.read(self.fat_off, -(-fatbytes // self.bps) * self.bps))
        self.items = []
        self.bitmap = None
        self.fixed = set()          # clusters that never move: root folder, bitmap, up-case table
        self.problems = []          # 1.1: what the scan could not read cleanly (the check refuses to repair then)
        self.scan()

    # -- clusters
    def coff(self, c):
        return self.heap + (c - 2) * self.cs
    def fat_next(self, c):
        return le32(self.fat, c * 4)
    def chain(self, first, nofat, size):
        if first < 2:
            return []
        n = -(-size // self.cs) if size else 0
        if nofat:
            return list(range(first, first + n))
        out, c = [], first
        seen = 0
        while 2 <= c < self.nclus + 2:
            out.append(c)
            seen += 1
            if seen > self.nclus:
                raise DefragError('a cluster chain loops (run chkdsk first)')
            c = self.fat_next(c)
            if c >= 0xFFFFFFF7:
                break
        return out
    def read_clusters(self, clusters):
        out = bytearray()
        for s, n in runs_of(clusters):
            out += self.dev.read(self.coff(s), n * self.cs)
        return out
    def is_used(self, c):
        i = c - 2
        return (self.bitmap[i >> 3] >> (i & 7)) & 1

    # -- the tree
    def scan(self):
        self.items = []
        rootc = self.chain(self.root, False, 0)
        self.fixed.update(rootc)
        rootdir = DirContent(rootc)
        data = self.read_clusters(rootc)
        bm_first = bm_len = None
        for o in range(0, len(data), 32):
            t = data[o]
            if t == 0x81:
                bm_first, bm_len = le32(data, o + 20), le64(data, o + 24)
            elif t == 0x82:
                up_first, up_len = le32(data, o + 20), le64(data, o + 24)
                self.fixed.update(self.chain(up_first, False, up_len))
            elif t == 0:
                break
        if bm_first is None:
            raise DefragError('no allocation bitmap in the root folder')
        bmc = self.chain(bm_first, False, bm_len)
        self.fixed.update(bmc)
        self.bitmap_clusters = bmc
        self.bitmap = bytearray(self.read_clusters(bmc)[:bm_len])
        self._walk(rootdir, data, '', 0)

    def _walk(self, dobj, data, prefix, depth):
        if depth > 64:
            raise DefragError('folders nested too deep under %s' % prefix)
        o = 0
        n = len(data)
        while o < n:
            t = data[o]
            if t == 0:
                break
            if t != 0x85:
                o += 32
                continue
            sec = data[o + 1]
            if sec < 2 or o + 32 * (sec + 1) > n:
                o += 32
                continue
            ents = data[o:o + 32 * (sec + 1)]
            if set_checksum(ents) != le16(ents, 2) or ents[32] != 0xC0:
                self.log('  skipped a damaged entry in %s/ (run chkdsk)' % prefix)
                self.problems.append('a damaged entry in %s/' % prefix)
                o += 32 * (sec + 1)
                continue
            attr = le16(ents, 4)
            st = 32
            namelen = ents[st + 3]
            name = ''
            for k in range(2, sec + 1):
                e = ents[32 * k: 32 * k + 32]
                if e[0] == 0xC1:
                    name += e[2:32].decode('utf-16-le', 'replace')
            name = name[:namelen]
            it = Item()
            it.path = prefix + '/' + name
            it.is_dir = bool(attr & 0x10)
            it.parent = dobj
            it.set_off = o
            it.set_len = 32 * (sec + 1)
            it.nofat = bool(ents[st + 1] & 2)
            it.first = le32(ents, st + 20)
            it.size = le64(ents, st + 24)
            it.clusters = self.chain(it.first, it.nofat, it.size) if ents[st + 1] & 1 else []
            if not it.nofat and it.size and len(it.clusters) * self.cs < it.size:
                self.log('  %s: its cluster chain is shorter than its size (run chkdsk) -- left alone' % it.path)
                self.problems.append('%s: cluster chain shorter than the file' % it.path)
                it.clusters = []
            self.items.append(it)
            if it.is_dir and it.clusters:
                sub = DirContent(it.clusters)
                it.dirobj = sub
                self._walk(sub, self.read_clusters(it.clusters)[:it.size], it.path, depth + 1)
            o += 32 * (sec + 1)

    # -- directory entries
    def dir_pos(self, dobj, off):
        """disk byte offset of byte `off` of a folder's data"""
        k, r = divmod(off, self.cs)
        return self.coff(dobj.clusters[k]) + r
    def read_set(self, it):
        out = bytearray()
        for p in range(it.set_off, it.set_off + it.set_len, 32):
            out += self.dev.read(self.dir_pos(it.parent, p), 32)
        return out
    def write_set(self, it, ents):
        # one write per stretch of the set that lies in one cluster
        p = it.set_off
        while p < it.set_off + it.set_len:
            k = p // self.cs
            end = min(it.set_off + it.set_len, (k + 1) * self.cs)
            self.dev.write(self.dir_pos(it.parent, p), bytes(ents[p - it.set_off:end - it.set_off]))
            p = end

    # -- allocation
    def bitmap_set(self, clusters, used):
        dirty = set()
        for c in clusters:
            i = c - 2
            if used:
                self.bitmap[i >> 3] |= 1 << (i & 7)
            else:
                self.bitmap[i >> 3] &= ~(1 << (i & 7)) & 0xff
            dirty.add((i >> 3) // self.bps)
        for s in sorted(dirty):
            o = s * self.bps
            chunk = bytes(self.bitmap[o:o + self.bps])
            k, r = divmod(o, self.cs)
            self.dev.write(self.coff(self.bitmap_clusters[k]) + r, chunk.ljust(self.bps, b'\0')[:self.bps]
                           if len(chunk) < self.bps else chunk)
    def fat_write(self, pairs):
        """pairs: [(cluster, value)]"""
        dirty = set()
        for c, v in pairs:
            struct.pack_into('<I', self.fat, c * 4, v)
            dirty.add((c * 4) // self.bps)
        for s in sorted(dirty):
            o = s * self.bps
            self.dev.write(self.fat_off + o, bytes(self.fat[o:o + self.bps]))
    def set_dirty(self, on):
        vbr = bytearray(self.dev.read(0, self.bps))
        f = le16(vbr, 0x6A)
        f = (f | 2) if on else (f & ~2)
        struct.pack_into('<H', vbr, 0x6A, f)
        self.dev.write(0, bytes(vbr))
        self.dev.flush()

    def check(self, fix=False):
        """1.1: compare the allocation bitmap with what the files and folders own.  Returns (leaked, unmarked, errors);
        fix=True gives the leaked clusters back, marks the unmarked ones used and clears the dirty mark -- only when
        there are no errors."""
        owner = bytearray(self.nclus + 2)          # 0 free, 1 owned once, 2 owned twice or more
        errors = list(self.problems)

        def own(clusters, what):
            for c in clusters:
                if not 2 <= c < self.nclus + 2:
                    errors.append('%s points outside the volume (cluster %d)' % (what, c))
                    return
                if owner[c]:
                    if owner[c] == 1:
                        errors.append('cluster %d belongs to two files (one is %s)' % (c, what))
                    owner[c] = 2
                else:
                    owner[c] = 1
        own(sorted(self.fixed), 'the root folder / bitmap / up-case table')
        for it in self.items:
            own(it.clusters, it.path)
        leaked, unmarked = [], []
        for c in range(2, self.nclus + 2):
            used = self.is_used(c)
            if used and not owner[c]:
                leaked.append(c)
            elif owner[c] and not used:
                unmarked.append(c)
        self.log('check: %d files and folders; %s; %s; %s' % (
            len(self.items),
            '%d clusters marked used but owned by nothing (%s)' % (len(leaked), human(len(leaked) * self.cs))
            if leaked else 'no lost clusters',
            '%d clusters in use but marked free' % len(unmarked) if unmarked else 'nothing in use marked free',
            '%d problem(s)' % len(errors) if errors else 'no damage found'))
        for e in errors[:20]:
            self.log('  PROBLEM: %s' % e)
        if fix:
            if errors:
                raise DefragError('the volume has damage this tool does not repair -- run "Check" on it in Explorer '
                                  '(or chkdsk /f) instead')
            if leaked:
                self.bitmap_set(leaked, False)
            if unmarked:
                self.bitmap_set(unmarked, True)
            self.dev.flush()
            self.set_dirty(False)
            self.dirty = False
            self.log('fixed: %d clusters given back, %d marked used; the volume is marked clean'
                     % (len(leaked), len(unmarked)))
        return leaked, unmarked, errors

    def free_runs(self):
        """sorted [(start, length)] of the free clusters"""
        out = []
        bm = self.bitmap
        n = self.nclus
        i = 0
        start = None
        nb = len(bm)
        while i < n:
            b = i >> 3
            if (i & 7) == 0 and b < nb:
                v = bm[b]
                if v == 0xFF:
                    if start is not None:
                        out.append((start + 2, i - start)); start = None
                    i += 8
                    continue
                if v == 0 and i + 8 <= n:
                    if start is None:
                        start = i
                    i += 8
                    continue
            if (bm[b] >> (i & 7)) & 1:
                if start is not None:
                    out.append((start + 2, i - start)); start = None
            elif start is None:
                start = i
            i += 1
        if start is not None:
            out.append((start + 2, n - start))
        return out

class FreeSpace:
    """the free runs, sorted by start (and by length, for the best fit); allocating and releasing keep them merged"""
    def __init__(self, runs):
        self.starts = [s for s, n in runs]
        self.lens = [n for s, n in runs]
        self.bylen = sorted((n, s) for s, n in runs)
    def total(self):
        return sum(self.lens)
    def largest(self):
        return self.bylen[-1][0] if self.bylen else 0
    def best_fit(self, n):
        k = bisect.bisect_left(self.bylen, (n, -1))
        return self.bylen[k][1] if k < len(self.bylen) else None
    def lowest_fit(self, n, below):
        for i, s in enumerate(self.starts):
            if s >= below:
                return None
            if self.lens[i] >= n and s + n <= below:
                return s
        return None
    def _del(self, i):
        self.bylen.pop(bisect.bisect_left(self.bylen, (self.lens[i], self.starts[i])))
        del self.starts[i]; del self.lens[i]
    def _ins(self, i, s, n):
        self.starts.insert(i, s); self.lens.insert(i, n)
        bisect.insort(self.bylen, (n, s))
    def take(self, start, n):
        i = bisect.bisect_right(self.starts, start) - 1
        s, l = self.starts[i], self.lens[i]
        assert i >= 0 and s <= start and start + n <= s + l, 'not free'
        self._del(i)
        if start + n < s + l:
            self._ins(i, start + n, s + l - start - n)
        if start > s:
            self._ins(i, s, start - s)
    def give(self, clusters):
        for s, n in runs_of(sorted(clusters)):
            i = bisect.bisect_left(self.starts, s)
            if i < len(self.starts) and s + n == self.starts[i]:
                n += self.lens[i]; self._del(i)
            if i > 0 and self.starts[i - 1] + self.lens[i - 1] == s:
                s = self.starts[i - 1]; n += self.lens[i - 1]; self._del(i - 1); i -= 1
            self._ins(i, s, n)

# --------------------------------------------------------------------------------------------------------------------
# the work

class Defrag:
    def __init__(self, vol, log=print, progress=lambda f, t='': None, verify=True, pack=True, folders=True, full=False):
        self.v, self.log, self.progress = vol, log, progress
        self.verify, self.pack, self.folders, self.full = verify, pack, folders, full
        self.stop = False
        self.moved = self.moved_bytes = self.failed = 0

    def report(self):
        v = self.v
        frag = [it for it in v.items if it.clusters and len(runs_of(it.clusters)) > 1]
        files = [it for it in v.items if not it.is_dir]
        dirs = [it for it in v.items if it.is_dir]
        free = FreeSpace(v.free_runs())
        self.log('%s: exFAT, %s clusters of %s, %s free in %d pieces (largest %s)' % (
            v.dev.name, '{:,}'.format(v.nclus), human(v.cs), human(free.total() * v.cs), len(free.starts),
            human(free.largest() * v.cs)))
        self.log('%d files, %d folders; %d fragmented (%d files, %d folders), %d pieces in all' % (
            len(files), len(dirs), len(frag), sum(1 for i in frag if not i.is_dir), sum(1 for i in frag if i.is_dir),
            sum(len(runs_of(i.clusters)) for i in frag)))
        if v.dirty:
            self.log('NOTE: the volume is marked dirty (not cleanly removed) -- run Windows\' "Check" (chkdsk /f) first')
        for it in sorted(frag, key=lambda i: -len(runs_of(i.clusters)))[:40]:
            self.log('  %5d pieces  %10s  %s%s' % (len(runs_of(it.clusters)), human(len(it.clusters) * v.cs), it.path,
                                                '/' if it.is_dir else ''))
        if len(frag) > 40:
            self.log('  ... and %d more' % (len(frag) - 40))
        return frag

    def _copy(self, src, dst_start):
        v = self.v
        n = len(src)
        done = 0
        for s, l in runs_of(src):
            pos = 0
            while pos < l:
                k = min(l - pos, max(1, (4 << 20) // v.cs))
                data = v.dev.read(v.coff(s + pos), k * v.cs)
                d = dst_start + done
                v.dev.write(v.coff(d), data)
                if self.verify:
                    back = v.dev.read(v.coff(d), k * v.cs)
                    if back != data:
                        raise DefragError('the copy read back differently at cluster %d -- the drive may be failing' % d)
                pos += k; done += k
        assert done == n

    def move(self, it, dst, free):
        """it -> clusters dst .. dst + n - 1 (free).  Data first, then the entry, then the old clusters go."""
        v = self.v
        n = len(it.clusters)
        old = list(it.clusters)
        new = list(range(dst, dst + n))
        ents = v.read_set(it)
        # the entry must still be the one scanned
        if le32(ents, 32 + 20) != it.first or set_checksum(ents) != le16(ents, 2):
            raise DefragError('%s changed since the scan' % it.path)
        free.take(dst, n)
        self._copy(old, dst)
        v.bitmap_set(new, True)
        if not it.nofat:
            v.fat_write([(c, c + 1) for c in new[:-1]] + [(new[-1], 0xFFFFFFFF)])
        v.dev.flush()
        struct.pack_into('<I', ents, 32 + 20, dst)
        struct.pack_into('<H', ents, 2, set_checksum(ents))
        v.write_set(it, ents)
        v.dev.flush()
        it.first = dst
        it.clusters[:] = new             # a folder's DirContent shares this list: its children follow it
        oldset = set(old) - set(new)
        v.bitmap_set(sorted(oldset), False)
        if not it.nofat:
            v.fat_write([(c, 0) for c in sorted(oldset)])
        v.dev.flush()
        free.give(oldset)
        self.moved += 1
        self.moved_bytes += n * v.cs

    def movable(self, it):
        return it.clusters and (self.folders or not it.is_dir) and not (set(it.clusters[:1]) & self.v.fixed)

    def run(self):
        v = self.v
        t0 = time.time()
        frag = [it for it in v.items if self.movable(it) and len(runs_of(it.clusters)) > 1]
        free = FreeSpace(v.free_runs())
        total = sum(len(it.clusters) for it in frag) or 1
        done = 0
        v.set_dirty(True)                  # cleared only when every move went through (a failure leaves it for chkdsk)
        if True:
            pending = []
            # 1. every fragmented item into the smallest free run that holds it (biggest items first)
            for it in sorted(frag, key=lambda i: -len(i.clusters)):
                if self.stop:
                    break
                dst = free.best_fit(len(it.clusters))
                if dst is None:
                    pending.append(it)
                    continue
                self.progress(done / total, it.path)
                self._try(it, dst, free)
                done += len(it.clusters)
            # 2. no room for some: pack the others toward the start, then try again
            if (pending and self.pack) or self.full:
                self._pack(free, pending)
                for it in sorted(pending, key=lambda i: len(i.clusters)):     # small first: they then fit anywhere
                    if self.stop:
                        break
                    dst = free.best_fit(len(it.clusters))
                    if dst is None:
                        if not it.nofat and self.inplace(it, free):
                            done += len(it.clusters)
                            continue
                        self.log('  no free run of %s for %s (largest %s, %s free in all)' % (
                            human(len(it.clusters) * v.cs), it.path, human(free.largest() * v.cs),
                            human(free.total() * v.cs)))
                        self.failed += 1
                        continue
                    self.progress(done / total, it.path)
                    self._try(it, dst, free)
                    done += len(it.clusters)
                if self.full and not self.stop:
                    self._pack(free, [])                   # what the gathering freed, packed too
            elif pending:
                for it in pending:
                    self.log('  no free run big enough for %s (packing is off)' % it.path)
                    self.failed += 1
        v.dev.flush()
        v.set_dirty(False)
        self.progress(1.0, '')
        dt = time.time() - t0
        self.log('done: %d moved (%s) in %d min %02d s%s%s' % (self.moved, human(self.moved_bytes), dt // 60, dt % 60,
                 ', %d could not be fixed' % self.failed if self.failed else '', ' -- stopped' if self.stop else ''))

    def _try(self, it, dst, free):
        try:
            self.move(it, dst, free)
            self.log('  moved %s%s (%s) to cluster %d' % (it.path, '/' if it.is_dir else '', human(len(it.clusters) * self.v.cs), dst))
        except DefragError as ex:
            self.failed += 1
            self.log('  NOT moved: %s: %s' % (it.path, ex))
            if 'changed since' not in str(ex):
                raise

    # -- in place: for a file bigger than any free run (an almost full iPod)
    def _window(self, it, free):
        """where to gather `it`: a run of len(it.clusters) clusters with no fixed clusters and as little of other files
        as possible.  The file's own pieces outside it become free as they move in, so the others can go there."""
        v = self.v
        n = len(it.clusters)
        fs, fl = free.starts, free.lens
        pref = [0]
        for l in fl:
            pref.append(pref[-1] + l)
        own = sorted(it.clusters)
        fixed = sorted(v.fixed)
        def free_in(a, b):
            lo, hi = bisect.bisect_left(fs, a), bisect.bisect_left(fs, b)
            t = pref[hi] - pref[lo]
            if hi > lo and fs[hi - 1] + fl[hi - 1] > b:
                t -= fs[hi - 1] + fl[hi - 1] - b
            if lo > 0 and fs[lo - 1] + fl[lo - 1] > a:
                t += min(fs[lo - 1] + fl[lo - 1], b) - a
            return t
        def count(lst, a, b):
            return bisect.bisect_left(lst, b) - bisect.bisect_left(lst, a)
        cands = set(r[0] for r in runs_of(it.clusters))
        for st, l in zip(fs, fl):
            cands.add(st); cands.add(st + l - n)
        best = None
        for t in cands:
            if t < 2 or t + n > v.nclus + 2 or count(fixed, t, t + n):
                continue
            inside_free, inside_own = free_in(t, t + n), count(own, t, t + n)
            foreign = n - inside_free - inside_own
            room = free.total() - inside_free + (n - inside_own)       # free outside + own pieces that leave
            if room >= foreign + 1 and (best is None or (foreign, t) < best):
                best = (foreign, t)
        return best

    def inplace(self, it, free):
        """gather `it` (a FAT-chain file or folder) into one run, front to back, a block at a time: whatever else sits
        at the next place moves out (to free space, which grows as the file's own old pieces are released), the next
        pieces are copied there, linked into the chain and their old clusters released.  Every step leaves a whole,
        readable file; if room runs out it stops with the file in fewer pieces than before."""
        v = self.v
        n = len(it.clusters)
        w = self._window(it, free)
        if w is None:
            return False
        foreign, t = w
        self.log('  gathering %s (%s) in place at cluster %d (%d clusters of other files move out of the way)' % (
            it.path, human(n * v.cs), t, foreign))
        owner = {}
        for o in v.items:
            if o is not it and o.clusters:
                for c in o.clusters:
                    if t <= c < t + n:
                        owner[c] = o
        def reserve():
            i = max(0, bisect.bisect_right(free.starts, t) - 1)
            take = []
            while i < len(free.starts) and free.starts[i] < t + n:
                a, b = max(free.starts[i], t), min(free.starts[i] + free.lens[i], t + n)
                if b > a:
                    take.append((a, b - a))
                i += 1
            for a, l in take:
                free.take(a, l)
        def unreserve():
            free.give([c for c in range(t, t + n) if not v.is_used(c)])
        reserve()
        ents = v.read_set(it)
        if le32(ents, 32 + 20) != it.first or set_checksum(ents) != le16(ents, 2):
            unreserve()
            raise DefragError('%s changed since the scan' % it.path)
        cl = it.clusters
        pos = {c: k for k, c in enumerate(cl)}
        EOC = 0xFFFFFFFF
        i, d = 0, t
        complete = False
        while True:
            if i >= n:
                complete = True
                break
            if self.stop:
                break
            self.progress(i / n, 'gathering ' + it.path)
            if cl[i] == d:
                i += 1; d += 1
                continue
            o = owner.get(d)
            if o is not None and o.clusters and d in o.clusters:
                # another file or folder sits here: it moves out whole
                if not self.movable(o):
                    self.log('  %s cannot move -- stopping here' % o.path); break
                dst = free.best_fit(len(o.clusters))
                if dst is None:
                    self.log('  no room to move %s out of the way -- stopping here' % o.path); break
                for c in o.clusters:
                    owner.pop(c, None)
                self._try(o, dst, free)
                reserve()
                ents = v.read_set(it)     # (a folder that moved may be this item's parent)
                continue
            j = pos.get(d)
            if j is not None:
                # d holds a later piece of this file: that one goes to a free cluster outside first
                x = free.best_fit(1)
                if x is None:
                    self.log('  no free cluster left to work with -- stopping here'); break
                free.take(x, 1)
                self._copy([d], x)
                v.fat_write([(x, v.fat_next(d))]); v.bitmap_set([x], True); v.dev.flush()
                v.fat_write([(cl[j - 1], x)]); v.dev.flush()
                v.bitmap_set([d], False); v.fat_write([(d, 0)]); v.dev.flush()
                cl[j] = x; del pos[d]; pos[x] = j
            k = 0
            lim = max(1, (4 << 20) // v.cs)
            while i + k < n and k < lim and d + k < t + n and not v.is_used(d + k) and d + k not in pos:
                k += 1
            if k == 0:
                if v.is_used(d) and d not in pos and owner.get(d) is None:
                    self.log('  cluster %d is used by nothing known (run chkdsk) -- stopping here' % d)
                    break
                continue
            src = cl[i:i + k]
            self._copy(src, d)
            new = list(range(d, d + k))
            v.fat_write([(c, c + 1) for c in new[:-1]] + [(new[-1], cl[i + k] if i + k < n else EOC)])
            v.bitmap_set(new, True)
            v.dev.flush()
            if i == 0:
                struct.pack_into('<I', ents, 32 + 20, d)
                struct.pack_into('<H', ents, 2, set_checksum(ents))
                v.write_set(it, ents)
                it.first = d
            else:
                v.fat_write([(cl[i - 1], d)])
            v.dev.flush()
            v.bitmap_set(src, False)
            v.fat_write([(c, 0) for c in src])
            v.dev.flush()
            for c in src:
                del pos[c]
            for m, c in enumerate(new):
                pos[c] = i + m
            cl[i:i + k] = new
            free.give([c for c in src if not (t <= c < t + n)])
            i += k; d += k
        unreserve()
        if complete:
            self.moved += 1
            self.moved_bytes += n * v.cs
            self.log('  gathered %s%s (%s) at cluster %d' % (it.path, '/' if it.is_dir else '', human(n * v.cs), t))
        else:
            self.log('  %s: now %d pieces' % (it.path, len(runs_of(cl))))
        return complete

    def _pack(self, free, pending):
        """move contiguous items down into the lowest free run that holds them, until a run is big enough"""
        v = self.v
        need = max((len(i.clusters) for i in pending if len(i.clusters) <= free.total()), default=0)
        if need == 0 and not self.full:
            return
        pend = set(id(i) for i in pending)
        order = sorted((it for it in v.items if self.movable(it) and id(it) not in pend and len(runs_of(it.clusters)) == 1),
                       key=lambda i: i.clusters[0])
        self.log('packing files toward the start to make room%s' % (' (full pack)' if self.full else
                 ' for %s' % human(need * v.cs)))
        for k, it in enumerate(order):
            if self.stop or (not self.full and free.largest() >= need):
                break
            dst = free.lowest_fit(len(it.clusters), it.clusters[0])
            if dst is None:
                continue
            self.progress(k / max(1, len(order)), 'packing ' + it.path)
            self._try(it, dst, free)

# --------------------------------------------------------------------------------------------------------------------

def gui():
    import tkinter as tk
    from tkinter import ttk, messagebox, scrolledtext
    w = tk.Tk()
    w.title('%s %s' % (APP, VERSION))
    w.geometry('760x540')
    top = ttk.Frame(w, padding=8); top.pack(fill='x')
    ttk.Label(top, text='iPod:').pack(side='left')
    drives = exfat_drives()
    names = ['%s  %s%s' % (d, lab or '(no name)', '  (iPod)' if ip else '') for d, lab, ip in drives]
    var = tk.StringVar(value=next((n for n, d in zip(names, drives) if d[2]), names[0] if names else ''))
    cb = ttk.Combobox(top, textvariable=var, values=names, width=40)
    cb.pack(side='left', padx=6)
    opts = ttk.Frame(w, padding=(8, 0)); opts.pack(fill='x')
    v_verify = tk.BooleanVar(value=True); v_pack = tk.BooleanVar(value=True)
    v_dirs = tk.BooleanVar(value=True); v_full = tk.BooleanVar(value=False)
    ttk.Checkbutton(opts, text='Read every copy back (safer)', variable=v_verify).pack(side='left')
    ttk.Checkbutton(opts, text='Pack files to make room', variable=v_pack).pack(side='left', padx=8)
    ttk.Checkbutton(opts, text='Folders too', variable=v_dirs).pack(side='left')
    ttk.Checkbutton(opts, text='Full pack (free space in one piece)', variable=v_full).pack(side='left', padx=8)
    txt = scrolledtext.ScrolledText(w, height=20, font=('Consolas', 9)); txt.pack(fill='both', expand=True, padx=8, pady=6)
    bar = ttk.Progressbar(w, maximum=1000); bar.pack(fill='x', padx=8)
    status = ttk.Label(w, text='', padding=(8, 2)); status.pack(fill='x')
    btns = ttk.Frame(w, padding=8); btns.pack(fill='x')
    state = {'job': None}
    lf = open(log_path(), 'a', encoding='utf-8')

    def log(s):
        lf.write(time.strftime('%H:%M:%S ') + s + '\n'); lf.flush()
        w.after(0, lambda: (txt.insert('end', s + '\n'), txt.see('end')))
    def prog(f, t=''):
        w.after(0, lambda: (bar.configure(value=int(f * 1000)), status.configure(text=t[-110:])))
    def target():
        s = var.get().strip()
        return s.split()[0] if s else ''
    def work(do_fix):
        if do_fix == 'check':
            return work_check()
        dev = None
        try:
            t = target()
            if not t:
                raise DefragError('no exFAT drive found -- connect the iPod (Rockbox: USB mode)')
            log('---- %s %s %s' % (APP, VERSION, time.strftime('%Y-%m-%d %H:%M')))
            dev = open_dev(t, do_fix)
            if do_fix:
                dev.lock()
            vol = Volume(dev, log)
            d = Defrag(vol, log, prog, v_verify.get(), v_pack.get(), v_dirs.get(), v_full.get())
            state['job'] = d
            frag = d.report()
            if do_fix:
                if vol.dirty:
                    raise DefragError('the volume is marked dirty: press "Check & fix" first (or run "Check" on it in '
                                      'Explorer / chkdsk %s /f)' % t)
                if not frag and not v_full.get():
                    log('nothing to do')
                else:
                    d.run()
        except DefragError as ex:
            log('ERROR: %s' % ex)
        except Exception as ex:
            import traceback
            log('ERROR: %s\n%s' % (ex, traceback.format_exc()))
        finally:
            if dev:
                dev.close()
            state['job'] = None
            w.after(0, lambda: [b.configure(state='normal') for b in (b_scan, b_fix, b_chk)])
    def work_check():
        dev = None
        try:
            t = target()
            if not t:
                raise DefragError('no exFAT drive found -- connect the iPod (Rockbox: USB mode)')
            log('---- %s %s %s: check %s' % (APP, VERSION, time.strftime('%Y-%m-%d %H:%M'), t))
            dev = open_dev(t, True)
            dev.lock()
            vol = Volume(dev, log)
            leaked, unmarked, errors = vol.check(False)
            if errors:
                log('not repaired: run "Check" on the drive in Explorer (or chkdsk %s /f)' % t)
            elif leaked or unmarked or vol.dirty:
                vol.check(True)
            else:
                log('nothing to fix')
        except DefragError as ex:
            log('ERROR: %s' % ex)
        except Exception as ex:
            import traceback
            log('ERROR: %s\n%s' % (ex, traceback.format_exc()))
        finally:
            if dev:
                dev.close()
            w.after(0, lambda: [b.configure(state='normal') for b in (b_scan, b_fix, b_chk)])

    def start(do_fix):
        if do_fix == 'check':
            if not messagebox.askyesno(APP, 'Check %s and repair lost space / the dirty mark?\n\nThe drive is locked '
                                       'for a minute or two; do not unplug the iPod.' % target()):
                return
            for b in (b_scan, b_fix, b_chk):
                b.configure(state='disabled')
            threading.Thread(target=work, args=('check',), daemon=True).start()
            return
        if do_fix and not messagebox.askyesno(APP, 'Defragment %s now?\n\nThe drive is locked while this runs; do not '
                                              'unplug the iPod.  Large moves take a while (about 1 minute per GB).' % target()):
            return
        for b in (b_scan, b_fix, b_chk):
            b.configure(state='disabled')
        threading.Thread(target=work, args=(do_fix,), daemon=True).start()
    def stop():
        if state['job']:
            state['job'].stop = True
            log('stopping after the current file ...')
    b_scan = ttk.Button(btns, text='Scan', command=lambda: start(False)); b_scan.pack(side='left')
    b_fix = ttk.Button(btns, text='Defragment', command=lambda: start(True)); b_fix.pack(side='left', padx=8)
    b_chk = ttk.Button(btns, text='Check & fix', command=lambda: start('check'))
    b_chk.pack(side='left', padx=(0, 8))
    ttk.Button(btns, text='Stop', command=stop).pack(side='left')
    if not drives:
        log('No exFAT drive found.  Connect the iPod (in Rockbox: USB mode) and restart this tool.')
    w.mainloop()

def main():
    a = sys.argv[1:]
    if not a:
        if os.name == 'nt' and not is_admin():
            params = '"%s"' % os.path.abspath(sys.argv[0]) if not getattr(sys, 'frozen', False) else ''
            exe = sys.executable
            if exe.lower().endswith('python.exe') and os.path.exists(exe[:-10] + 'pythonw.exe'):
                exe = exe[:-10] + 'pythonw.exe'
            if ctypes.windll.shell32.ShellExecuteW(None, 'runas', exe, params, None, 1) <= 32:
                import tkinter as tk
                from tkinter import messagebox
                r = tk.Tk(); r.withdraw()
                messagebox.showerror(APP, '%s needs administrator rights to read the drive directly.' % APP)
                r.destroy()
            return
        return gui()
    def opt(name):
        return a[a.index(name) + 1] if name in a and a.index(name) + 1 < len(a) else None
    if '--check' in a:
        target = opt('--check')
        fix = '--fix' in a
        if fix and '--yes' not in a:
            sys.exit('add --yes to really repair')
        dev = open_dev(target, fix)
        try:
            if fix:
                dev.lock()
            vol = Volume(dev, print)
            leaked, unmarked, errors = vol.check(False)
            if fix and (leaked or unmarked or vol.dirty) and not errors:
                vol.check(True)
            sys.exit(1 if errors else 0)
        except DefragError as ex:
            sys.exit('ERROR: %s' % ex)
        finally:
            dev.close()
    target = opt('--scan') or opt('--defrag')
    if not target:
        sys.exit(__doc__)
    fix = '--defrag' in a
    if fix and '--yes' not in a:
        sys.exit('add --yes to really move files')
    dev = open_dev(target, fix)
    try:
        if fix:
            dev.lock()
        vol = Volume(dev, print)
        d = Defrag(vol, print, lambda f, t='': None, '--no-verify' not in a, '--no-pack' not in a,
                   '--files-only' not in a, '--full' in a)
        frag = d.report()
        if fix:
            if vol.dirty:
                sys.exit('the volume is marked dirty: check it first (chkdsk /f, fsck.exfat)')
            if frag or '--full' in a:
                d.run()
            if d.failed:
                sys.exit(1)
    except DefragError as ex:
        sys.exit('ERROR: %s' % ex)
    finally:
        dev.close()

if __name__ == '__main__':
    main()

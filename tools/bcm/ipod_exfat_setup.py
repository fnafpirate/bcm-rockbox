#!/usr/bin/env python3
"""iPod exFAT Setup -- one-click formatter for an iPod Video (5G / 5.5G) running
the bcm-rockbox fork.

What it does (after a full backup of the boot area):
  1. keeps Apple's firmware partition (partition 1, type 0x00 -- the boot ROM
     only boots from this raw "image directory" format) but replaces the Apple
     OS image in it with the Rockbox bootloader only (like ipodpatcher -wf)
  2. replaces the data partition with one standard exFAT partition (type
     0x07) filling the rest of the disk -- no 2 TB / 4 GB limits, opens on
     any Windows / macOS / Linux PC
  3. installs Rockbox (rockbox.zip) on it, plus your old settings, themes,
     fonts and VideoCore files from the current /.rockbox
  4. reads everything back and checks it

Python 3.8+ standard library only.  Windows: run it, it asks for admin rights.
Put the iPod in Rockbox USB mode (or disk mode) first.

Blank-drive mode ("New drive"): builds everything from nothing on any disk --
an SD card / CF card / iPod hard drive in a USB adapter, or an iPod in disk
mode -- so it can be put straight into the iPod: partition table, firmware
partition with the Rockbox bootloader, exFAT partition with Rockbox.

Command line (also used for testing on a disk image):
  ipod_exfat_setup.py                       -> window
  ipod_exfat_setup.py --list                -> show detected iPods
  ipod_exfat_setup.py --disk N|--image F --rockbox-zip Z --bootloader B
                      [--keep-dir D] [--backup F] [--label L] [--cluster-kb 128] --yes
  ipod_exfat_setup.py --new [--model 5.5g|5g] (--disk N|--image F) --rockbox-zip Z --bootloader B
                      [--keep-dir SAVED_ROCKBOX_FOLDER] [--label L] [--cluster-kb 128] --yes
  ipod_exfat_setup.py --restore BACKUP (--disk N|--image F) --yes
  ipod_exfat_setup.py --disks               -> all disks a new layout could go on
"""
import base64, datetime, io, json, os, queue, shutil, struct, sys, threading, time, zipfile, zlib

VERSION = '1.2'
ALIGN = 4 << 20            # partition start and cluster heap: 4 MiB aligned (flash friendly)
CHUNK = 4 << 20

# --------------------------------------------------------------------------
# constants

APPLE_STOP_SIGN = b"".join([
    b'{{~~  /-----\\   ',
    b'{{~~ /       \\  ',
    b'{{~~|         | ',
    b'{{~~| S T O P | ',
    b'{{~~|         | ',
    b'{{~~ \\       /  ',
    b'{{~~  \\-----/   ',
    b'Copyright(C) 200',
    b'1 Apple Computer',
    b', Inc.----------',
    b'----------------',
    b'----------------',
    b'----------------',
    b'----------------',
    b'----------------',
    b'---------------',
])
assert len(APPLE_STOP_SIGN) == 255

# the up-case table recommended by the exFAT specification (5836 bytes,
# checksum 0xE619D30D), as Windows writes it
UPCASE_B64 = """
eNqt2HXQFseWBvCe7hlc3uDWb5+GF280uLsHCxAcgoUQIIQQQgjB3d3d3Z0P+HB3d0IIIQR3P/uQ3dqqe9mquza/6j+emjoz3dPV
Y0J4QgolfBGIGCKmiCViizgirogn4osEIqEIiU9EIpFYJBFJRTKRXKQQKUUqkVqkEVqEhREkrEgr0omISC8yiIwik8gssoiswols
IrvIIXKKXCK3+FTkEXlFPpFfFBAFRSFRWBQRRUUxUVyUECVFKVFalBFlRTlRXlQQFUUlUVlUEZ+JqqKaqC5qiJric1FL1BZ1xBei
rqgn6osGoqFoJBqLJqLp/6r+Z9FN/CK6ix6ip+gleos+oq/oJ/qLAWKgGCQGiyFiqBgmhosRYqQYJUaLMWKsGCfGiwliopgkJosp
YqqYJqaLGWKmmCVmizlirpgn5osFYqFYJBaLJWKpWCaWixVipVglVos1Yq1YJ9aLDWKj2CQ2iy0iSmwV28R2ES12iJ1il9gt9oi9
Yp/YLw6Ig+KQOCyOiKPimDguToiT4pQ4Lc6Is+KcOC8uiIvikrgsroir4tr/sP7FP9V38TD9ngQfYkAsiAPxIAGEIBEkgWSQAlJB
GggDQVqIQAbIBFnAQXbI6eXyckMeyAcFvIJQGIpCcSgJpaEslIeKXiWoAlWhOtSEWlAH6kJ9aAiNoSk0gxbQClpDG2gL7aEDdIRO
0Bm6eD/Bz/AL9PBKy15eb+gL/b0BMMgb7A2BYd5wb4Q30hsFY7yx3nNvvDfBmwhF5RRvqjfNIznDmwmzYS7M9xbAIm+xt8RbCsu9
FbDKW+2tgXWwwdsIm70tXhRs81540d4Ob6e3y9vt7UHb5+1HO+gdQjsCx+AEnIIzcA4uwCUY7l2F63ADbsItuA134C7ch4feI+8x
2lN4jvO9hNfwFt4DFr/8sPkQA2JBHIgHCSAEiSAJJIMUkArSQBhIWpkWIpABMkEWcJAdckJuyCPzynwyvywgC8qWrrAsLIvKVq64
LCFLQmlZRpaV5aACVIIqUBWqyxqyJuaov1dbfpiZunKEV1+O9BrKRrKxbCLHeF/KZnKs10K2lK3kV3ICZqmNbObaynayvZzqdZDf
yWne97IT5qqz/FF2kT/JrvJn2U22cN1lDznf6yV7y0VeX9lP9pcD5HKvjPwwY2XlMDlcjpAj5Si5wRsjx8pxcrycICfKSXKynCKn
ymlyupwhZ8pZcracI+fKeXK+XCAXykVysVwil8plcrlcIVfKVXK1XCPXynVyvdwgN8pNcrPcIqPkVrlNbpfRcofcKXfJ3XKP3Cv3
yf3ygDwoD8nD8og8Ko/J4/KEPClPydPyjDwrz8nz8oK8KC/Jy/KKvCqvyevyV3lD/iZvyt/lLfmHvC3/lHfkX/KuvCfvywfyoXwk
H8sn8ql8Jp/LF/KlfCVfyzfyrXwn30vGrd9TUinlq0DFUDFVLBVbxVFxVTwVXyVQCVVIfaISqcQqiUqqkqnkKoVKqVKp1CqN0iqs
jCJlVVqVTkVUepVBZVSZVGaVRWVVTmVT2VUOlVPlUrnVpyqPyqvyqfyqgCqoCqnCqogqqoqp4qqEKqlKqdKqjCqryqnyqoKqqCqp
yqqK+kxVVdVUdVVD1VSfq1qqtqqjvlB1VT1VXzVQDVUj1Vg1UU3Vl6qZaq5aqJaqlfpKtVZfqzbqG9VWtVPt1beqg/pOdVTfq07q
B9VZ/ai6qJ9UV/VOvVesuqseqqfqpXqrPqqv6qf6qwFqoBqkBqshaqgapoarEWqkGqVGqzFqrBqnxqsJaqKapCarKWqqmqamqxlq
ppqlZqs5aq6ap+arBWqhWqQWqyU41r8faeV/o37Of1E/9O+zH1PH1Ql1Up1Sp9UZdVadU+fhIlyGq3AdbsBNuAW34Q7chfvwUD1S
r9QT9VQ9U8/VC3ilXsPb/7gGAjd+6Svf9wM/hh/Tj+XH9uP4cf14fnw/gZ/QD/mf+In8xH4SP6mfzE/up/BT+qn81H4aX/th3/jk
Wz+tn86P+On9DH5GP5Of2c/iZ/Wdn83P7uf4P9f/q/41hWbQAlpBa2gDbaE9dICO0Ak6QxfoCt2gO/SE3n4fv6/fz+/vD/AH+oP8
wTAUhsNIGA1jYTxMhMkwFabDTJgNc2E+LITFsBSWw0pYDWthPWyEzRAF2yDa3wG7YA/sgwNwCI5AtH8cTsJpOAvn4SJchqtwHW7A
TbgFt+EO3IX78BAew1N4Di/hNbyF94DXvkACLm8QA2JBHIgHCSAEiSBJkDRIFiQPUgQpg1RB6iBNoINwYAIKbJA2SBdEgvRBhiBj
kCnIHGQJsgYuyBZkD3IEOYNcQe7g0yBPkDfIF+QPCgQFg0JB4aBIUDQoFhQPSgQlg1JB6aBMUDYoF5QPKgQVg0pB5aBK8FlQNagW
VA9qBDWDz4NaQe2gTvBFUDeoF9QPGgQNg0ZB46BJ0PT/9fjMz5M3d911D91T99K9dR/dV/fT/fUAPVAP0oP1ED1UD9PD9Qg9Uo/S
o/UYPVaP0+P1BD1RT9KT9RQ9VU/T0/UMPVPP0rP1HD1Xz9Pz9QK9UC/Si/USvVQv08v1Cr1Sr9Kr9Rq9Vq/T6/UGvVFv0pv1Fh2l
t+pteruO1jv0Tr1L79Z79F69T+/XB/RBfUgf1kf0UX1MH9cn9El9Sp/WZ/RZfU6f1xf0RX1JX9ZX9FV9TV/Xv+ob+jd9U/+ub+k/
9G39p76j/9J39T19Xz/QD/Uj/Vg/0U/1M/1cv9Av9Sv9Wr/Rb/U7/V7zh5f7D4/3MBZpOAbEgjgQDxJACBJBEkgGKSAVpIEPG0Fa
iEAGyARZwEF2yAm5IQ/kgwJQCIpAMSgBpaAMlIMKUAmqQFWoDjWhFtSBulAfGkJjaArNoAW0gtbQBtpCe+gAHaETdIYu0BW6QXfo
Cb2hL/SHgTAYhsJwGAmjYSyMD08ITwxPCk8OTwlPDU8LTw/PCM+E2TAX5sNCWAxLYTmshNWwFtbDRtgMUbANomEn7Ia9sB8OwmE4
CsfhJJyGs3AeLsJluArX4QbchFtwG+7AXbgPD+ExPIXn8BJeh9+E34bfhd+HORzLxDZxTFwTz8Q3CUxC8885hUlpUpnUJo3RJplJ
/g85bIzJZDKbLCarcSabyW5yfJQLmIKmkClsipiippgp/lGuYCqaSqayqWI+M+VM+X/IVU01U93UMzVNA1PLNDJ1TBNTF7k+ckPk
xsitzdemjfnGtDXtTHvz7Ud5s9li9psD5qA5ZC6aS+aleWXumL/Ma/PGdDc9zEAzyAw2Q8xQM8wMNyM+yhPNJDPZTDFTzTQz3cz4
KC80i8xis8QsNcvMcrPio7zRbDKrTZRZa9aZ9WbD3/lDn6LMVrPNbDfRZofZaXaZ3WaP2Wv2/Wdfd5kj5qg5Zs6bC+akOWVOmzPm
rDn3d/4wjsvmirlqrpnb5k9zw/xmbpq75pb54+/8YXx3zT1z3zwwD80j89g8MU/NM/PcvPh7/B/G/sS8M+8N48MenzekyKeAYlBM
ikWxKQ7FpXgUnxJQQgrRJ5SIElMSSkrJKDmloJSUilJTGtJY+oaILKWldBSh9JSBMlImykxZKCs5ykbZKQflpFyUmz6lPJSX8lF+
KkAFqRAVpiJUlIpRcSpBJakUlaYyVJbKUXmqQBWpElWmKvQZVaVqVJ1qUE36nGpRbapDX1Bdqkf1qQE1pEbUmJpQU/qSmlFzakEt
qRV9Ra3pa2pD31Bbakft6VvqQN9RR/qeOtEP1Jl+pC70E3Wln6kb/ULdqQf1pF7Um/pQX+pH/WkADaRBNJiG0FAaRsNpBI2kUTSa
xtBYGkfjaQJNpEk0mabQVJpG02kGzaRZNJvm0FyaR/NpAS2kRbSYltBSWkbLaQWtpFW0mtbQWlpH62kDbaRNtJm2UBRtpW20naJp
B+2kXbSb9tBe2kf76QAdpEN0mI7QUTpGx+kEnaRTdJrO0Fk6R+fpAl2kS3SZrtBVukbX6Ve6Qb/RTfqdbtEfdJv+pDv0F92le3Sf
HtBDekSP6Qk9pWf0nF7QS3pFr+kNvaV39J4Yv3U8K62yvg1sDBvTxrKxbRwb18az8W0Cm9CG7Cc2kU1sk9ikNplNblPYlDaVTW3T
WG3D1liy1qa16WzEprcZbEabyWa2WWxW62w2m93msDltLpvbfmrz2Lw2n81vC9iCtpAtbIvYoraYLW5L2JK2lC1ty9iytpwtbyvY
iraSrWyr2M9QV81WtzVsTfu5rWVr2zr2C1vX1rP1bQPb0DayjW0T29R+aZvZ5raFbWlb2a9sa/u1bWO/sW1tO9vefvsv9/e0vWxv
2weYK6v1kQ2RjZFNkc2RLZGoyNbItsj2SHRkR2RnZFdkd2RPZG9kX2R/5EDkYORQ5HDkSORo5FiEuVxMgd8g0innu8DFcDFdLBfb
xXFxXTwX3yVwCV3IfeISucQuiUvqkrnkLoVL6VK51C6N03hQGkfOurQunYu49C6Dy+gyucwui8vqnMvmsrsmrik0c81dC9fStXJf
wdfwDbRz7d23roP7znV037tO7gf40XVxP7mu7mfXzf3iurserif0hr7QHwbCYBgKw2EkjIaxMB4mwmSYCtNhJsyGuTAfFsJiWArL
YSWshrWwHjbCZoiCbRANO2E37IX9cBAOw1E4DifhNJyF83ARLsNVuA434Kb73d1yf7jb7k93x/3l7rp77r574B66R+6xe+Keumfu
uXvhXrpX7rV74966d+69YzczNCs0OzQnNDc0LzQ/tCC0MLQotDi0JLQ0tCy0PLQitDK0KrQ6tCa0NrQutD60IbQxtCm0ObQlFBXa
GtoW2h6KDu0I7QztCu0O7Qkxpz5pOS2n4win5wyckTNxZs7CWdlxNs7OOTgn5+Lc/Cnn4bycj/NzAS7IhbgBN+RG3JibcFP+kptx
c27BLbkVf8Wt+Wtuw99wW27H7flb7sDfcUf+njvxD9yZf+Qu/BN35Z+5G//C3bkH9+Re3Jv7cF/ux/15AA/kQTyYh/BQHsbDeQSP
5FE8msfwWB7H43kCT+RJPJmn8FSextN5Bs/kWTyb5/BcnsfzeQEv5EW8mJfwUl7Gy3kFr+RVvJrX8Fpex+t5A2/kTbyZt3AUb+Vt
vJ2jeQfv5F28m/fwXt7H+/kAH+RDfJiP8FE+xsf5BJ/kU3yaz/BZPsfn+QJf5Et8ma/wVb7G1/lXvsG/8U3+nW/xH3yb/+Q7/Bff
5Xt8nx/wQ37Ej/kJP+Vn/Jxf8Et+xa/5Db/ld/yemf8NWc4hRg==
"""

def upcase_table():
    raw = zlib.decompress(base64.b64decode(''.join(UPCASE_B64.split())))
    assert len(raw) == 5836 and table_checksum(raw) == 0xE619D30D
    up = list(range(65536))
    vals = struct.unpack('<%dH' % (len(raw) // 2), raw)
    i = idx = 0
    while i < len(vals) and idx < 65536:
        if vals[i] == 0xffff and i + 1 < len(vals):
            idx += vals[i + 1]; i += 2
        else:
            up[idx] = vals[i]; idx += 1; i += 1
    return raw, up

def table_checksum(data):
    c = 0
    for b in data:
        c = (((c & 1) << 31) | (c >> 1)) + b & 0xffffffff
    return c

def boot_checksum(region, ss):
    c = 0
    for i, b in enumerate(region[:11 * ss]):
        if i in (106, 107, 112):
            continue
        c = (((c & 1) << 31) | (c >> 1)) + b & 0xffffffff
    return c

def set_checksum(ents):
    c = 0
    for i, b in enumerate(ents):
        if i in (2, 3):
            continue
        c = (((c & 1) << 15) | (c >> 1)) + b & 0xffff
    return c

def utf16_units(name):
    b = name.encode('utf-16-le')
    return list(struct.unpack('<%dH' % (len(b) // 2), b))

def name_hash(units, up):
    h = 0
    for u in units:
        u = up[u]
        for b in (u & 0xff, u >> 8):
            h = (((h & 1) << 15) | (h >> 1)) + b & 0xffff
    return h

def le16(b, o): return struct.unpack_from('<H', b, o)[0]
def le32(b, o): return struct.unpack_from('<I', b, o)[0]
def le64(b, o): return struct.unpack_from('<Q', b, o)[0]
def align_up(x, a): return -(-x // a) * a
def human(n):
    for u in ('B', 'KB', 'MB', 'GB', 'TB'):
        if n < 1000 or u == 'TB':
            return ('%.2f %s' % (n, u)) if u != 'B' else '%d B' % n
        n /= 1000.0

class SetupError(Exception):
    pass

def log_path():
    for d in (os.path.join(os.path.expanduser('~'), 'Documents'), os.path.expanduser('~'),
              os.path.dirname(os.path.abspath(sys.argv[0]))):
        if os.path.isdir(d):
            return os.path.join(d, 'ipod_exfat_setup_log.txt')
    return 'ipod_exfat_setup_log.txt'

def to_logfile(text):
    try:
        with open(log_path(), 'a', encoding='utf-8') as f:
            f.write('%s  %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), text))
    except OSError:
        pass

def write_at(disk, off, data):
    """write bytes at any offset (read-modify-write of partial sectors)"""
    ss = disk.sector
    a = off - off % ss
    end = off + len(data)
    b = align_up(end, ss)
    if a == off and b == end:
        return disk.write(off, bytes(data))
    buf = bytearray(b - a)
    if a != off:
        buf[:ss] = disk.read(a, ss)
    if b != end:
        buf[-ss:] = disk.read(b - ss, ss)
    buf[off - a: off - a + len(data)] = data
    for pos in range(0, len(buf), CHUNK):
        disk.write(a + pos, bytes(buf[pos:pos + CHUNK]))

# --------------------------------------------------------------------------
# disks

class ImageDisk:
    """a disk image file (testing); 'sector' = the sector size a PC would see"""
    def __init__(self, path, sector=512, writable=True):
        self.path = path
        self.f = open(path, 'r+b' if writable else 'rb')
        self.f.seek(0, 2)
        self.size = self.f.tell()
        self.sector = sector
        self.name = os.path.basename(path)

    def read(self, off, n):
        assert off % self.sector == 0 and n % self.sector == 0, (off, n)
        self.f.seek(off)
        d = self.f.read(n)
        if len(d) != n:
            raise SetupError('short read at %d' % off)
        return d

    def write(self, off, data):
        assert off % self.sector == 0 and len(data) % self.sector == 0, (off, len(data))
        if off + len(data) > self.size:
            raise SetupError('write past the end of the disk')
        self.f.seek(off)
        self.f.write(data)

    def flush(self):
        self.f.flush(); os.fsync(self.f.fileno())

    def volumes(self):
        return []

    def prepare_write(self, log=print):
        pass

    def lock_volumes(self):
        pass

    def finish(self):
        self.flush()

    def close(self):
        self.f.close()

if os.name == 'nt':
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
    FILE_SHARE_RW = 3
    OPEN_EXISTING = 3
    INVALID_HANDLE = ctypes.c_void_p(-1).value
    IOCTL_DISK_GET_DRIVE_GEOMETRY_EX = 0x000700A0
    IOCTL_STORAGE_QUERY_PROPERTY = 0x002D1400
    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS = 0x00560000
    IOCTL_DISK_UPDATE_PROPERTIES = 0x00070140
    IOCTL_DISK_IS_WRITABLE = 0x00070024
    IOCTL_DISK_GET_DISK_ATTRIBUTES = 0x000700F0
    IOCTL_DISK_SET_DISK_ATTRIBUTES = 0x0007C0F4
    ERROR_WRITE_PROTECT = 19
    FSCTL_LOCK_VOLUME, FSCTL_UNLOCK_VOLUME, FSCTL_DISMOUNT_VOLUME = 0x00090018, 0x0009001C, 0x00090020
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                    ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                    ctypes.c_void_p]
    k32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                             ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k32.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong, ctypes.c_void_p, wintypes.DWORD]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.FindFirstVolumeW.restype = wintypes.HANDLE
    k32.FindFirstVolumeW.argtypes = [wintypes.LPWSTR, wintypes.DWORD]
    k32.FindNextVolumeW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD]
    k32.FindVolumeClose.argtypes = [wintypes.HANDLE]
    k32.GetVolumePathNamesForVolumeNameW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD,
                                                     ctypes.POINTER(wintypes.DWORD)]
    k32.FlushFileBuffers.argtypes = [wintypes.HANDLE]

    def _open(path, access):
        h = k32.CreateFileW(path, access, FILE_SHARE_RW, None, OPEN_EXISTING, 0, None)
        if h in (None, INVALID_HANDLE):
            raise OSError(ctypes.get_last_error(), 'cannot open %s' % path)
        return h

    def _ioctl(h, code, inbuf=None, outsize=0):
        out = ctypes.create_string_buffer(outsize) if outsize else None
        ret = wintypes.DWORD(0)
        inb = ctypes.create_string_buffer(inbuf, len(inbuf)) if inbuf else None
        ok = k32.DeviceIoControl(h, code, inb, len(inbuf) if inbuf else 0, out, outsize, ctypes.byref(ret), None)
        if not ok:
            raise OSError(ctypes.get_last_error(), 'DeviceIoControl %08x failed' % code)
        return out.raw[:ret.value] if out is not None else b''

    def is_admin():
        try:
            return bool(ctypes.windll.shell32.IsUserAnAdmin())
        except Exception:
            return False

    def disk_volumes(number):
        """[(volume_guid_path, [mount paths])] of the volumes on PhysicalDrive<number>"""
        out = []
        buf = ctypes.create_unicode_buffer(1024)
        h = k32.FindFirstVolumeW(buf, 1024)
        if h in (None, INVALID_HANDLE):
            return out
        try:
            while True:
                vol = buf.value
                try:
                    vh = _open(vol.rstrip('\\'), 0)
                    try:
                        ext = _ioctl(vh, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, None, 8 + 24 * 16)
                    finally:
                        k32.CloseHandle(vh)
                    n = le32(ext, 0)
                    disks = [le32(ext, 8 + 24 * i) for i in range(n)]
                    if number in disks:
                        names = ctypes.create_unicode_buffer(4096)
                        ln = wintypes.DWORD(0)
                        paths = []
                        if k32.GetVolumePathNamesForVolumeNameW(vol, names, 4096, ctypes.byref(ln)):
                            paths = [p for p in names[:ln.value].split('\0') if p]
                        out.append((vol, paths))
                except OSError:
                    pass
                if not k32.FindNextVolumeW(h, buf, 1024):
                    break
        finally:
            k32.FindVolumeClose(h)
        return out

    BUS_NAMES = {1: 'SCSI', 2: 'ATAPI', 3: 'ATA', 7: 'USB', 0xb: 'SATA', 0xc: 'SD', 0xd: 'MMC',
                 0x11: 'NVMe', 0x8: 'RAID', 0xa: 'SAS'}

    def system_disks():
        """disk numbers holding the Windows folder (never offered)"""
        out = set()
        win = os.environ.get('SystemRoot', 'C:\\Windows')[:2]
        try:
            vh = _open('\\\\.\\' + win, 0)
            try:
                ext = _ioctl(vh, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, None, 8 + 24 * 16)
                for i in range(le32(ext, 0)):
                    out.add(le32(ext, 8 + 24 * i))
            finally:
                k32.CloseHandle(vh)
        except OSError:
            out.add(0)          # cannot tell: be safe, never offer disk 0
        return out

    class WinDisk:
        def __init__(self, number, writable=True):
            self.number = number
            self.h = _open(r'\\.\PhysicalDrive%d' % number, GENERIC_READ | (GENERIC_WRITE if writable else 0))
            try:
                g = _ioctl(self.h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, None, 256)
                if le64(g, 24) == 0:
                    raise OSError(21, 'no medium')
            except OSError:
                k32.CloseHandle(self.h)
                raise
            self.sector = le32(g, 20)
            self.size = le64(g, 24)
            self.name = 'Disk %d' % number
            try:
                q = struct.pack('<III', 0, 0, 0)
                d = _ioctl(self.h, IOCTL_STORAGE_QUERY_PROPERTY, q, 1024)
                def s(off):
                    o = le32(d, off)
                    return d[o:d.index(b'\0', o)].decode('ascii', 'replace').strip() if o else ''
                self.name = ('%s %s' % (s(12), s(16))).strip() or self.name
            except (OSError, ValueError):
                pass
            self.locked = []
            self.locked_names = set()
            self.bus, self.removable = None, None
            try:
                self.bus = le32(d, 28)
                self.removable = bool(d[10])
            except (NameError, IndexError, struct.error):
                pass

        def _seek(self, off):
            if not k32.SetFilePointerEx(self.h, off, None, 0):
                raise SetupError('seek to %d failed (error %d)' % (off, ctypes.get_last_error()))

        def read(self, off, n):
            assert off % self.sector == 0 and n % self.sector == 0
            self._seek(off)
            buf = ctypes.create_string_buffer(n)
            got = wintypes.DWORD(0)
            if not k32.ReadFile(self.h, buf, n, ctypes.byref(got), None) or got.value != n:
                raise SetupError('read at %d failed (error %d)' % (off, ctypes.get_last_error()))
            return buf.raw

        def write(self, off, data):
            assert off % self.sector == 0 and len(data) % self.sector == 0
            if off + len(data) > self.size:
                raise SetupError('write past the end of the disk')
            self._seek(off)
            buf = ctypes.create_string_buffer(bytes(data), len(data))
            done = wintypes.DWORD(0)
            if not k32.WriteFile(self.h, buf, len(data), ctypes.byref(done), None) or done.value != len(data):
                err = ctypes.get_last_error()
                if err == ERROR_WRITE_PROTECT:
                    raise SetupError('the disk is write-protected (SD card lock switch?)')
                raise SetupError('write at %d failed (Windows error %d) -- is a drive on that disk still open '
                                 'in Explorer or another program?' % (off, err))

        def flush(self):
            k32.FlushFileBuffers(self.h)

        def prepare_write(self, log=print):
            """a brand-new disk can be 'offline' or 'read-only' in Windows; an SD card can be locked"""
            try:
                a = _ioctl(self.h, IOCTL_DISK_GET_DISK_ATTRIBUTES, None, 16)
                attrs = le64(a, 8)
                if attrs & 3:
                    log('  the disk is %s in Windows -- switching that off' %
                        ' and '.join(x for b, x in ((1, 'offline'), (2, 'read-only')) if attrs & b))
                    _ioctl(self.h, IOCTL_DISK_SET_DISK_ATTRIBUTES, struct.pack('<IB3xQQ16x', 40, 0, 0, 3))
                    try:
                        _ioctl(self.h, IOCTL_DISK_UPDATE_PROPERTIES)
                    except OSError:
                        pass
            except OSError:
                pass
            try:
                _ioctl(self.h, IOCTL_DISK_IS_WRITABLE)
            except OSError as ex:
                if ex.errno == ERROR_WRITE_PROTECT:
                    raise SetupError('the disk is write-protected.  On an SD card, slide the small lock switch '
                                     'away from "LOCK" and try again.')

        def volumes(self):
            return disk_volumes(self.number)

        def lock_volumes(self, log=print):
            """lock + dismount every volume on the iPod so Windows lets us write"""
            for vol, paths in self.volumes():
                if vol in self.locked_names:
                    continue
                vh = _open(vol.rstrip('\\'), GENERIC_READ | GENERIC_WRITE)
                for attempt in range(20):
                    try:
                        _ioctl(vh, FSCTL_LOCK_VOLUME)
                        break
                    except OSError:
                        if attempt == 5:
                            log('  %s is in use -- forcing it offline' % (paths[0] if paths else vol))
                            try:
                                _ioctl(vh, FSCTL_DISMOUNT_VOLUME)
                            except OSError:
                                pass
                        time.sleep(0.5)
                else:
                    k32.CloseHandle(vh)
                    raise SetupError('cannot lock %s -- close any Explorer window or program using the '
                                     'iPod and try again' % (paths[0] if paths else vol))
                _ioctl(vh, FSCTL_DISMOUNT_VOLUME)
                self.locked.append(vh)
                self.locked_names.add(vol)

        def finish(self):
            self.flush()
            try:
                _ioctl(self.h, IOCTL_DISK_UPDATE_PROPERTIES)
            except OSError:
                pass

        def close(self):
            for vh in self.locked:
                try:
                    _ioctl(vh, FSCTL_UNLOCK_VOLUME)
                except OSError:
                    pass
                k32.CloseHandle(vh)
            self.locked = []
            k32.CloseHandle(self.h)
else:
    def is_admin():
        return os.geteuid() == 0 if hasattr(os, 'geteuid') else True

# --------------------------------------------------------------------------
# the iPod's layout

class Ipod:
    """what is on the disk now"""
    def __init__(self, disk):
        self.disk = disk
        ss = disk.sector
        self.mbr_sector = bytearray(disk.read(0, max(512, ss)))
        mbr = self.mbr_sector
        if mbr[510:512] != b'\x55\xaa':
            raise SetupError('no partition table (MBR) on this disk')
        self.parts = []
        for k in range(4):
            e = mbr[0x1be + 16 * k: 0x1be + 16 * (k + 1)]
            self.parts.append((e[4], le32(e, 8), le32(e, 12)))
        t, start, size = self.parts[0]
        if t != 0 or not start or not size:
            raise SetupError('partition 1 is not an iPod firmware partition (type 0x00)')
        # partition table units: the 5.5G boot ROM reads it in 2048-byte
        # units, older iPods in 512; the Apple header tells which
        self.unit = None
        for unit in (2048, 512):
            off = start * unit
            if off + 4096 > disk.size:
                continue
            hdr = self._read(off, 4096)
            if hdr[:255] == APPLE_STOP_SIGN and hdr[0x100:0x104] == b']ih[':
                self.unit = unit
                break
        if not self.unit:
            raise SetupError('partition 1 has no Apple firmware header -- not an iPod?')
        self.fw_start = start * self.unit
        self.fw_size = size * self.unit
        hdr = self._read(self.fw_start, 4096)
        self.fw_version = le16(hdr, 0x10a)
        self.diroffset = le32(hdr, 0x104) + 0x200
        d = self._read(self.fw_start + self.diroffset, 400)
        self.images = []
        p = 0
        while p + 40 <= len(d) and d[p:p + 4] in (b'!ATA', b'DNAN'):
            self.images.append(dict(type=d[p + 4:p + 8][::-1].decode('latin1'), id=le32(d, p + 8),
                                    devOffset=le32(d, p + 12), len=le32(d, p + 16), addr=le32(d, p + 20),
                                    entryOffset=le32(d, p + 24), chksum=le32(d, p + 28), vers=le32(d, p + 32),
                                    loadAddr=le32(d, p + 36)))
            p += 40
        osos = [i for i, im in enumerate(self.images) if im['type'] == 'osos']
        if not osos:
            raise SetupError('no OS image in the firmware partition')
        self.osos = osos[0]
        im = self.images[self.osos]
        # where image data starts: one sector after the partition start (the
        # sector size the boot ROM uses); find it by the stored checksum
        self.fwoffset = None
        for cand in (self.fw_start + 2048, self.fw_start + 512, self.fw_start):
            if cand + im['devOffset'] + im['len'] > self.fw_start + self.fw_size:
                continue
            if self._checksum(cand + im['devOffset'], im['len']) == im['chksum']:
                self.fwoffset = cand
                break
        if self.fwoffset is None:
            raise SetupError('the OS image in the firmware partition does not match its checksum -- '
                             'not touching this iPod')
        later = [x['devOffset'] for x in self.images if x['devOffset'] > im['devOffset']]
        limit = min(later) if later else (self.fw_start + self.fw_size - self.fwoffset)
        self.osos_room = limit - im['devOffset']
        self.boot_has_rockbox = b'Rockbox' in self._read(self.fwoffset + im['devOffset'],
                                                         min(align_up(im['len'], 4096), 1 << 20))
        # the current data partition
        self.data_fs = None
        for t, s, n in self.parts[1:]:
            if t and s:
                off = s * self.unit
                if off + 4096 <= disk.size:
                    b = self._read(off, 4096)
                    self.data_fs = ('exFAT' if b[3:11] == b'EXFAT   ' else
                                    'FAT32' if b[82:87] == b'FAT32' else 'type 0x%02x' % t)
                break

    def _read(self, off, n):
        ss = self.disk.sector
        a = off - off % ss
        return self.disk.read(a, align_up(off + n - a, ss))[off - a: off - a + n]

    def _checksum(self, off, n):
        c = 0
        pos = 0
        while pos < n:
            k = min(CHUNK, n - pos)
            c = (c + sum(self._read(off + pos, k))) & 0xffffffff
            pos += k
        return c

    def describe(self):
        im = self.images[self.osos]
        return ('%s, %s, partition table in %d-byte units, firmware partition %s, OS image %s%s, data: %s'
                % (self.disk.name, human(self.disk.size), self.unit, human(self.fw_size), human(im['len']),
                   ' (Rockbox bootloader in it)' if self.boot_has_rockbox else '', self.data_fs or 'none'))

# --------------------------------------------------------------------------
# a new firmware partition, built from nothing
#
# What the boot ROM (iPod Video flash ROM, checked by disassembly) needs:
#   * partition table entry 1: type 0x00, in units of the ROM's sector size
#     (2048 bytes on the 5.5G, 512 on the 5G)
#   * the first 124 bytes of the partition = Apple's "stop sign" text
#   * u16 at 0x108 >= 0x100 and u16 at 0x10a == 3 (header version 3); the u32
#     at 0x104 + 0x200 = where the image directory is
#   * directory: 40-byte entries "!ATA" + type; image data at partition start
#     + one ROM sector + devOffset
#   * an "rsrc" entry must exist or the ROM shows an error instead of booting
#     (its contents are never read by the ROM)
#   * "osos" is loaded to 'addr', its byte sum must equal 'chksum', then the
#     ROM jumps to it ('entryOffset' 0)
# No "hibe" (the ROM would try to resume a hibernation image) and no "aupd".

MODELS = {'5.5g': 2048, '5g': 512}
FW_START_UNITS = 63                  # where iTunes puts it
FW_SIZE = 32 << 20
OSOS_DEV, OSOS_ROOM = 0x4800, 1 << 20

def disk_check_new(disk, unit):
    if disk.size < (1 << 30):
        raise SetupError('this disk is too small (%s)' % human(disk.size))
    if disk.size // unit > 0xffffffff + FW_SIZE // unit:
        raise SetupError('this disk is too big for the iPod partition table (max %s with %d-byte units)'
                         % (human(0xffffffff * unit), unit))

def create_blank(disk, unit, boot, log=print):
    """partition table + firmware partition with the bootloader; setup() then
    adds the exFAT partition"""
    disk_check_new(disk, unit)
    fw_start = FW_START_UNITS * unit
    fw = bytearray(FW_SIZE)
    fw[0:255] = APPLE_STOP_SIGN
    fw[0x100:0x104] = b']ih['
    struct.pack_into('<IHH', fw, 0x104, 0x4000, 0x010c, 3)
    rsrc = bytes(4096)
    ents = [(b'osos', OSOS_DEV, boot, 0x10000000, 0xffffffff),
            (b'rsrc', OSOS_DEV + OSOS_ROOM, rsrc, 0, 0)]
    d = 0x4200
    for typ, dev, data, addr, load in ents:
        struct.pack_into('<4s4sIIIIIIII', fw, d, b'!ATA', typ[::-1], 0, dev, len(data), addr, 0,
                         sum(data) & 0xffffffff, 0xb012, load)
        o = unit + dev
        fw[o:o + len(data)] = data
        d += 40
    log('writing a new partition table and firmware partition (%d-byte units)' % unit)
    # clear old partition tables (MBR / GPT) at the front and the backup GPT at the end
    write_at(disk, 0, bytes(fw_start))
    tail = min(1 << 20, disk.size - fw_start - FW_SIZE)
    write_at(disk, disk.size // disk.sector * disk.sector - tail, bytes(tail))
    for pos in range(0, FW_SIZE, CHUNK):
        write_at(disk, fw_start + pos, fw[pos:pos + CHUNK])
    mbr = bytearray(max(512, disk.sector))
    mbr[440:444] = os.urandom(4)
    mbr[0x1be:0x1ce] = struct.pack('<B3sB3sII', 0, b'\x00\x02\x00', 0, b'\xfe\xff\xff',
                                   FW_START_UNITS, FW_SIZE // unit)
    mbr[510:512] = b'\x55\xaa'
    write_at(disk, 0, bytes(mbr[:512]))
    disk.flush()

def new_drive(disk, unit, rockbox_zip, bootloader, keep_dir=None, label='IPOD', cluster=128 * 1024,
              log=print, progress=lambda f: None):
    boot = load_bootloader(bootloader)
    if len(boot) > OSOS_ROOM:
        raise SetupError('bootloader too big')
    zipfile.ZipFile(rockbox_zip).close()
    disk.prepare_write(log)
    if os.name == 'nt':
        disk.lock_volumes(log)
    else:
        disk.lock_volumes()
    create_blank(disk, unit, boot, log)
    return setup(disk, rockbox_zip, bootloader, None, keep=bool(keep_dir), keep_dir=keep_dir, label=label,
                 cluster=cluster, log=log, progress=progress)

def list_disks():
    """[(number, WinDisk-info dict)] of every disk; 'ok' = may be offered for a new layout"""
    out = []
    if os.name != 'nt':
        return out
    sysd = system_disks()
    for n in range(64):
        try:
            d = WinDisk(n, writable=False)
        except OSError:
            continue
        try:
            letters = [p for v, ps in d.volumes() for p in ps]
            external = d.bus in (7, 0xc, 0xd) or bool(d.removable)
            try:
                head = d.read(0, d.sector)
                blank = not any(head)
            except (SetupError, OSError):
                blank = False
            out.append((n, dict(name=d.name, size=d.size, sector=d.sector, bus=BUS_NAMES.get(d.bus, str(d.bus)),
                                letters=letters, system=n in sysd, ok=n not in sysd, internal=not external,
                                blank=blank)))
        finally:
            d.close()
    out.sort(key=lambda x: (x[1]['internal'], x[0]))
    return out

def find_ipods():
    """[(disk_number, Ipod or error string, disk)]"""
    out = []
    if os.name != 'nt':
        return out
    for n in range(32):
        try:
            d = WinDisk(n, writable=False)
        except OSError:
            continue
        try:
            out.append((n, Ipod(d)))
        except (SetupError, OSError, AssertionError):
            pass
        finally:
            d.close()
    return out

# --------------------------------------------------------------------------
# bootloader file

def load_bootloader(path):
    raw = open(path, 'rb').read()
    if len(raw) < 1024:
        raise SetupError('%s is too small for a bootloader' % path)
    if raw[4:8] in (b'ipvd', b'ipv5'):
        body = raw[8:]
        if (5 + sum(body)) & 0xffffffff != struct.unpack('>I', raw[:4])[0]:
            raise SetupError('%s: checksum wrong (damaged download?)' % path)
        return body
    raise SetupError('%s is not an iPod Video bootloader (.ipod file for "ipvd")' % path)

# --------------------------------------------------------------------------
# the files to install

class Node:
    def __init__(self, name, is_dir, size=0, opener=None, src=''):
        self.name = name; self.is_dir = is_dir; self.size = size; self.opener = opener; self.src = src
        self.children = {}          # upper-cased name -> Node
        self.first = 0; self.nclus = 0; self.crc = 0

class Tree:
    def __init__(self, up):
        self.up = up
        self.root = Node('', True)

    def key(self, name):
        return ''.join(chr(self.up[u]) if u < 65536 else chr(u) for u in [ord(c) for c in name])

    def add(self, path, size=0, opener=None, is_dir=False, src='', replace=True):
        parts = [p for p in path.replace('\\', '/').split('/') if p]
        node = self.root
        for i, p in enumerate(parts):
            check_name(p)
            k = self.key(p)
            last = i == len(parts) - 1
            if k in node.children:
                c = node.children[k]
                if last and not is_dir:
                    if c.is_dir:
                        raise SetupError('%s is both a file and a folder' % path)
                    if replace:
                        c.size, c.opener, c.src = size, opener, src
                    return
                if not c.is_dir:
                    raise SetupError('%s is both a file and a folder' % path)
                node = c
            else:
                c = Node(p, is_dir or not last, size if last and not is_dir else 0,
                         opener if last and not is_dir else None, src)
                node.children[k] = c
                node = c

    def walk(self, node=None, path=''):
        node = node or self.root
        for c in sorted(node.children.values(), key=lambda n: n.name.lower()):
            yield path + '/' + c.name, c
            if c.is_dir:
                for x in self.walk(c, path + '/' + c.name):
                    yield x

def check_name(n):
    if len(utf16_units(n)) > 255:
        raise SetupError('name too long: %s' % n)
    if any(ord(c) < 0x20 or c in '"*/:<>?\\|' for c in n) or n in ('.', '..'):
        raise SetupError('name not allowed on exFAT: %r' % n)

KEEP_SKIP_DIRS = ('codecs', 'rocks')                       # always fresh from rockbox.zip
KEEP_SKIP_FILES = ('rockbox.ipod', 'dircache.dat', '.dircache')

def keep_skip(rel):
    parts = rel.replace('\\', '/').split('/')
    low = [p.lower() for p in parts]
    if low[0] in KEEP_SKIP_DIRS:
        return True
    if low[-1] in KEEP_SKIP_FILES or (low[-1].startswith('database_') and low[-1].endswith('.tcd')):
        return True
    return False

def build_tree(up, rockbox_zip, keep_dir=None, log=print):
    """rockbox.zip on top of the kept /.rockbox files"""
    t = Tree(up)
    nkeep = 0
    if keep_dir and os.path.isdir(keep_dir):
        for dirpath, dirnames, filenames in os.walk(keep_dir):
            rel = os.path.relpath(dirpath, keep_dir)
            rel = '' if rel == '.' else rel.replace('\\', '/')
            dirnames[:] = [d for d in dirnames if not keep_skip((rel + '/' + d).lstrip('/'))]
            if rel:
                t.add('/.rockbox/' + rel, is_dir=True)
            for fn in filenames:
                r = (rel + '/' + fn).lstrip('/')
                if keep_skip(r):
                    continue
                p = os.path.join(dirpath, fn)
                t.add('/.rockbox/' + r, os.path.getsize(p), (lambda p=p: open(p, 'rb')), src='kept')
                nkeep += 1
    zf = zipfile.ZipFile(rockbox_zip)
    names = zf.namelist()
    if '.rockbox/rockbox.ipod' not in names:
        raise SetupError('%s is not a Rockbox build (.rockbox/rockbox.ipod missing)' % rockbox_zip)
    nzip = 0
    for info in zf.infolist():
        if info.filename.endswith('/'):
            t.add('/' + info.filename, is_dir=True)
        else:
            t.add('/' + info.filename, info.file_size, (lambda i=info: zf.open(i)), src='zip')
            nzip += 1
    log('  %d files from %s, %d kept from your old /.rockbox' % (nzip, os.path.basename(rockbox_zip), nkeep))
    rb = t.root.children.get(t.key('.rockbox'))
    if not rb or t.key('vc') not in rb.children:
        log('  NOTE: no VideoCore files (/.rockbox/vc) -- video playback needs them; copy them onto '
            'the iPod afterwards')
    return t, zf

# --------------------------------------------------------------------------
# exFAT

def dostime(dt):
    return (((dt.year - 1980) << 25) | (dt.month << 21) | (dt.day << 16) |
            (dt.hour << 11) | (dt.minute << 5) | (dt.second // 2))

class ExfatPlan:
    def __init__(self, part_off, part_len, ss, cs, label, up_raw, up):
        self.part_off, self.ss, self.cs = part_off, ss, cs
        self.spc = cs // ss
        self.vollen = part_len // ss
        self.label = label[:11]
        self.up_raw, self.up = up_raw, up
        a = ALIGN // ss
        self.fatoff = (1 << 20) // ss
        nclus = (self.vollen - self.fatoff) // self.spc
        for _ in range(3):
            self.fatlen = align_up((nclus + 2) * 4, ss) // ss
            self.heap = align_up(self.fatoff + self.fatlen, a)
            nclus = min((self.vollen - self.heap) // self.spc, 0xfffffff5 - 2)
        self.nclus = nclus
        self.fatlen = align_up((nclus + 2) * 4, ss) // ss
        assert self.fatoff + self.fatlen <= self.heap and nclus > 16
        self.bm_bytes = (nclus + 7) // 8
        now = datetime.datetime.now()
        self.stamp = dostime(now)
        off = now.astimezone().utcoffset()
        q = int(off.total_seconds() // 900) if off is not None else 0
        self.utcoff = 0x80 | (q & 0x7f)

    def clus_off(self, c):
        return self.part_off + self.heap * self.ss + (c - 2) * self.cs

    def entries(self, node):
        n = 0
        for c in node.children.values():
            n += 2 + -(-len(utf16_units(c.name)) // 15)
        return n

    def layout(self, tree):
        """assign clusters: bitmap, up-case, root, folders, files -- all in a row"""
        nxt = 2
        self.c_bitmap = nxt; self.bm_clus = -(-self.bm_bytes // self.cs); nxt += self.bm_clus
        self.c_upcase = nxt; self.up_clus = -(-len(self.up_raw) // self.cs); nxt += self.up_clus
        root = tree.root
        root.first = nxt; root.nclus = max(1, -(-(32 * (3 + self.entries(root))) // self.cs)); nxt += root.nclus
        order = []
        for path, n in tree.walk():
            if n.is_dir:
                n.first = nxt; n.nclus = max(1, -(-(32 * self.entries(n)) // self.cs)); nxt += n.nclus
                order.append(n)
        files = []
        for path, n in tree.walk():
            if not n.is_dir:
                if n.size:
                    n.first = nxt; n.nclus = -(-n.size // self.cs); nxt += n.nclus
                else:
                    n.first = 0; n.nclus = 0
                files.append((path, n))
        self.used_end = nxt
        if nxt - 2 > self.nclus:
            raise SetupError('the files do not fit')
        self.dirs = [root] + order
        self.files = files
        return self

    def dir_bytes(self, node, is_root):
        out = bytearray()
        if is_root:
            e = bytearray(32); e[0] = 0x83
            lab = utf16_units(self.label)
            e[1] = len(lab)
            if lab:
                struct.pack_into('<%dH' % len(lab), e, 2, *lab)
            out += e
            e = bytearray(32); e[0] = 0x81
            struct.pack_into('<IQ', e, 20, self.c_bitmap, self.bm_bytes); out += e
            e = bytearray(32); e[0] = 0x82
            struct.pack_into('<I', e, 4, table_checksum(self.up_raw))
            struct.pack_into('<IQ', e, 20, self.c_upcase, len(self.up_raw)); out += e
        for c in sorted(node.children.values(), key=lambda n: n.name.lower()):
            units = utf16_units(c.name)
            nn = -(-len(units) // 15)
            s = bytearray(32 * (2 + nn))
            s[0] = 0x85; s[1] = 1 + nn
            struct.pack_into('<HHIII', s, 4, 0x10 if c.is_dir else 0x20, 0, self.stamp, self.stamp, self.stamp)
            s[22] = s[23] = s[24] = self.utcoff
            length = c.nclus * self.cs if c.is_dir else c.size
            s[32] = 0xc0
            s[33] = 0x01 | (0x02 if c.first else 0)       # AllocationPossible, NoFatChain (contiguous)
            s[35] = len(units)
            struct.pack_into('<H', s, 36, name_hash(units, self.up))
            struct.pack_into('<QIIQ', s, 40, length, 0, c.first, length)
            for k in range(nn):
                o = 64 + 32 * k
                s[o] = 0xc1
                part = units[15 * k: 15 * k + 15]
                struct.pack_into('<%dH' % len(part), s, o + 2, *part)
            struct.pack_into('<H', s, 2, set_checksum(s))
            out += s
        n = node.nclus * self.cs
        assert len(out) <= n
        return bytes(out) + bytes(n - len(out))

    def boot_region(self, serial, percent):
        ss = self.ss
        bs = bytearray(ss)
        bs[0:3] = b'\xeb\x76\x90'; bs[3:11] = b'EXFAT   '
        struct.pack_into('<QQIIIIIIHHBBBBB', bs, 64, self.part_off // ss, self.vollen, self.fatoff,
                         self.fatlen, self.heap, self.nclus, self.dirs[0].first, serial, 0x0100, 0,
                         ss.bit_length() - 1, self.spc.bit_length() - 1, 1, 0x80, percent)
        bs[120:510] = b'\xf4' * 390
        bs[510:512] = b'\x55\xaa'
        region = bytearray(12 * ss)
        region[0:ss] = bs
        for k in range(1, 9):
            region[(k + 1) * ss - 4:(k + 1) * ss] = b'\x00\x00\x55\xaa'
        region[11 * ss:12 * ss] = struct.pack('<I', boot_checksum(region, ss)) * (ss // 4)
        return bytes(region)

    def write(self, disk, log, progress):
        ss, cs = self.ss, self.cs
        total = self.fatlen * ss + (self.used_end - 2) * cs
        done = [0]
        def step(n):
            done[0] += n
            progress(done[0] / total)
        # FAT: zeroed, then the chains of bitmap, up-case table and root folder
        log('  writing the FAT (%s)' % human(self.fatlen * ss))
        head = bytearray(min(self.fatlen * ss, CHUNK))
        struct.pack_into('<II', head, 0, 0xfffffff8, 0xffffffff)
        for first, n in ((self.c_bitmap, self.bm_clus), (self.c_upcase, self.up_clus),
                         (self.dirs[0].first, self.dirs[0].nclus)):
            for i in range(n):
                struct.pack_into('<I', head, 4 * (first + i), first + i + 1 if i + 1 < n else 0xffffffff)
        fat_off = self.part_off + self.fatoff * ss
        pos = 0
        zero = bytes(CHUNK)
        while pos < self.fatlen * ss:
            k = min(CHUNK, self.fatlen * ss - pos)
            write_at(disk, fat_off + pos, bytes(head[:k]) if pos == 0 else zero[:k])
            pos += k; step(k)
        # cluster heap, front to back in one stream
        log('  writing the allocation bitmap, up-case table, folders and %d files' % len(self.files))
        w = Streamer(disk, self.clus_off(2), step)
        used = self.used_end - 2
        bm = bytearray(self.bm_clus * cs)
        bm[:used // 8] = b'\xff' * (used // 8)
        for c in range(used // 8 * 8, used):
            bm[c // 8] |= 1 << (c % 8)
        w.put(bytes(bm))
        w.put(self.up_raw + bytes(self.up_clus * cs - len(self.up_raw)))
        for i, d in enumerate(self.dirs):
            w.put(self.dir_bytes(d, i == 0))
        for path, n in self.files:
            if not n.size:
                continue
            got = 0; crc = 0
            with n.opener() as f:
                while True:
                    b = f.read(CHUNK)
                    if not b:
                        break
                    crc = zlib.crc32(b, crc); got += len(b)
                    w.put(b)
            if got != n.size:
                raise SetupError('%s changed while copying' % path)
            n.crc = crc
            w.put(bytes(n.nclus * cs - n.size))
        w.close()
        # boot regions last: until here the partition is not an exFAT volume
        serial = struct.unpack('<I', os.urandom(4))[0]
        percent = min(100, used * 100 // self.nclus)
        region = self.boot_region(serial, percent)
        write_at(disk, self.part_off + 12 * ss, region)
        write_at(disk, self.part_off, region)

class Streamer:
    def __init__(self, disk, off, step):
        self.disk, self.off, self.step = disk, off, step
        self.buf = bytearray()
    def put(self, b):
        self.buf += b
        if len(self.buf) >= CHUNK:
            n = len(self.buf) // CHUNK * CHUNK
            self.disk.write(self.off, bytes(self.buf[:n]))
            self.off += n; self.step(n)
            del self.buf[:n]
    def close(self):
        if self.buf:
            n = align_up(len(self.buf), self.disk.sector)
            self.disk.write(self.off, bytes(self.buf) + bytes(n - len(self.buf)))
            self.step(len(self.buf))
            self.buf = bytearray()

# --------------------------------------------------------------------------
# independent read-back check of the new volume

def verify_exfat(disk, part_off, tree, up, log):
    def rd(off, n):
        ss = disk.sector
        a = off - off % ss
        return disk.read(a, align_up(off + n - a, ss))[off - a: off - a + n]
    bs = rd(part_off, 512)
    if bs[3:11] != b'EXFAT   ':
        raise SetupError('verify: no exFAT boot sector')
    ss = 1 << bs[108]; cs = ss << bs[109]
    for base in (0, 12):
        reg = rd(part_off + base * ss, 12 * ss)
        if le32(reg, 11 * ss) != boot_checksum(reg, ss):
            raise SetupError('verify: boot region checksum')
    fatoff, heap, nclus, root = le32(bs, 80), le32(bs, 88), le32(bs, 92), le32(bs, 96)
    def coff(c): return part_off + heap * ss + (c - 2) * cs
    def fat(c): return le32(rd(part_off + fatoff * ss + 4 * c, 4), 0)
    rootclus = []
    c = root
    while 2 <= c <= nclus + 1 and len(rootclus) < 1000:
        rootclus.append(c); c = fat(c)
    if c != 0xffffffff:
        raise SetupError('verify: root folder chain')
    rootdata = b''.join(rd(coff(c), cs) for c in rootclus)
    seen = [0, 0]
    def walk(data, node, path):
        found = {}
        i = 0
        while i < len(data):
            t = data[i]
            if t == 0:
                break
            if t == 0x85:
                sec = data[i + 1]
                raw = data[i:i + 32 * (sec + 1)]
                if le16(raw, 2) != set_checksum(raw) or raw[32] != 0xc0:
                    raise SetupError('verify: damaged entry in %s' % (path or '/'))
                nlen = raw[35]
                units = []
                for k in range(2, sec + 1):
                    units += struct.unpack_from('<15H', raw, 32 * k + 2)
                units = list(units[:nlen])
                if name_hash(units, up) != le16(raw, 36):
                    raise SetupError('verify: name hash in %s' % (path or '/'))
                name = struct.pack('<%dH' % nlen, *units).decode('utf-16-le')
                found[name] = (le16(raw, 4), raw[33], le32(raw, 52), le64(raw, 56))
                i += 32 * (sec + 1)
            else:
                i += 32
        want = {c.name: c for c in node.children.values()}
        if set(found) != set(want):
            raise SetupError('verify: %s lists %s' % (path or '/', sorted(set(found) ^ set(want))[:5]))
        for name, (attr, flags, first, length) in found.items():
            c = want[name]
            if bool(attr & 0x10) != c.is_dir or first != c.first:
                raise SetupError('verify: %s/%s' % (path, name))
            if c.is_dir:
                seen[0] += 1
                walk(rd(coff(first), length), c, path + '/' + name)
            else:
                seen[1] += 1
                if length != c.size:
                    raise SetupError('verify: size of %s/%s' % (path, name))
                crc = 0; pos = 0
                while pos < length:
                    k = min(CHUNK, length - pos)
                    crc = zlib.crc32(rd(coff(first) + pos, k), crc); pos += k
                if crc != c.crc:
                    raise SetupError('verify: contents of %s/%s differ' % (path, name))
    walk(rootdata, tree.root, '')
    log('  read back %d folders and %d files: all correct' % tuple(seen))

# --------------------------------------------------------------------------
# backup / restore of the boot area

def make_backup(ipod, path, log):
    disk = ipod.disk
    head = dict(magic='ipod-exfat-setup backup', version=1, disk_size=disk.size, unit=ipod.unit,
                fw_start=ipod.fw_start, fw_size=ipod.fw_size, mbr_len=len(ipod.mbr_sector),
                made=datetime.datetime.now().isoformat(timespec='seconds'))
    hb = json.dumps(head).encode()
    crc = 0
    with open(path + '.part', 'wb') as f:
        f.write(b'IPODBKP1' + struct.pack('<I', len(hb)) + hb)
        f.write(ipod.mbr_sector); crc = zlib.crc32(ipod.mbr_sector, crc)
        pos = 0
        while pos < ipod.fw_size:
            k = min(CHUNK, ipod.fw_size - pos)
            b = disk.read(ipod.fw_start + pos, k)
            f.write(b); crc = zlib.crc32(b, crc); pos += k
        f.write(struct.pack('<I', crc))
    os.replace(path + '.part', path)
    log('  boot area saved to %s (%s)' % (path, human(os.path.getsize(path))))

def restore_backup(disk, path, log=print):
    with open(path, 'rb') as f:
        if f.read(8) != b'IPODBKP1':
            raise SetupError('not a backup made by this tool')
        head = json.loads(f.read(struct.unpack('<I', f.read(4))[0]))
        if head['disk_size'] != disk.size:
            raise SetupError('this backup is from a disk of a different size')
        mbr = f.read(head['mbr_len'])
        fw = f.read(head['fw_size'])
        crc, = struct.unpack('<I', f.read(4))
    if zlib.crc32(fw, zlib.crc32(mbr)) != crc:
        raise SetupError('backup file is damaged')
    disk.lock_volumes(log) if os.name == 'nt' else disk.lock_volumes()
    for pos in range(0, len(fw), CHUNK):
        disk.write(head['fw_start'] + pos, fw[pos:pos + CHUNK])
    disk.write(0, mbr)
    disk.finish()
    log('restored the partition table and firmware partition from %s' % path)
    log('(the music partition itself is not in the backup: format it again with iTunes or this tool)')

# --------------------------------------------------------------------------
# the whole job

def keep_source(disk, log):
    """the old /.rockbox folder, if Windows has the iPod's partition open"""
    for vol, paths in disk.volumes():
        for p in paths:
            cand = os.path.join(p, '.rockbox')
            if os.path.isdir(cand):
                return cand
    return None

def setup(disk, rockbox_zip, bootloader, backup_path, keep=True, keep_dir=None, label='IPOD',
          cluster=128 * 1024, log=print, progress=lambda f: None):
    up_raw, up = upcase_table()
    ipod = Ipod(disk)
    log('iPod: ' + ipod.describe())
    boot = load_bootloader(bootloader)
    if len(boot) > ipod.osos_room:
        raise SetupError('the bootloader does not fit the firmware partition')
    unit = ipod.unit
    # 1. keep the old settings: copy /.rockbox to the PC first
    if keep and not keep_dir and backup_path:
        src = keep_source(disk, log)
        if src:
            keep_dir = os.path.splitext(backup_path)[0] + '_rockbox_folder'
            log('copying your current %s to %s' % (src, keep_dir))
            if os.path.exists(keep_dir):
                shutil.rmtree(keep_dir)
            shutil.copytree(src, keep_dir, ignore=lambda d, names: [n for n in names
                            if keep_skip(os.path.relpath(os.path.join(d, n), src))])
        else:
            log('no open /.rockbox folder on the iPod found -- installing fresh settings')
    tree, zf = build_tree(up, rockbox_zip, keep_dir if keep else None, log)
    # 2. backup of the boot area
    if backup_path:
        log('saving a backup of the partition table and firmware partition')
        make_backup(ipod, backup_path, log)
    # 3. new layout
    disk_end = disk.size // unit * unit
    fw_end = ipod.fw_start + ipod.fw_size
    part_off = align_up(fw_end, ALIGN)
    part_len = (disk_end - part_off) // ALIGN * ALIGN
    if part_len // unit > 0xffffffff:
        part_len = 0xffffffff * unit // ALIGN * ALIGN
    # Rockbox looks for the volume at start * 512, * 1024, * 2048 bytes in
    # turn; nothing that looks like a boot sector may sit at the earlier ones
    # (the firmware partition holds a FAT16 resource image)
    def looks_like_boot(b):
        return b[510:512] == b'\x55\xaa' and (b[3:11] == b'EXFAT   ' or b[82:87] == b'FAT32' or
                                              b[54:59] in (b'FAT12', b'FAT16'))
    for _ in range(64):
        clash = [j for j in (1, 2) if j * 512 < unit and
                 looks_like_boot(ipod._read(part_off // unit * 512 * j, 512))]
        if not clash:
            break
        log('  (moving the partition start: a boot sector sits where Rockbox would look first)')
        part_off += ALIGN
        part_len -= ALIGN
    ss = unit                       # what a PC sees through Rockbox's USB mode
    plan = ExfatPlan(part_off, part_len, ss, cluster, label, up_raw, up).layout(tree)
    log('new exFAT partition: %s at %s, %d KB clusters' % (human(part_len), human(part_off), cluster // 1024))
    # 4. write
    disk.prepare_write(log)
    log('taking the iPod offline in Windows' if os.name == 'nt' else 'writing')
    if os.name == 'nt':
        disk.lock_volumes(log)
    else:
        disk.lock_volumes()
    plan.write(disk, log, progress)
    # partition table: keep partition 1 (firmware), one exFAT partition after it
    mbr = bytearray(ipod.mbr_sector)
    for k in (1, 2, 3):
        mbr[0x1be + 16 * k: 0x1be + 16 * (k + 1)] = bytes(16)
    mbr[0x1ce:0x1de] = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x07, b'\xfe\xff\xff',
                                   part_off // unit, part_len // unit)
    disk.write(0, bytes(mbr))
    log('  partition table updated')
    # bootloader: replaces the OS image (Apple OS + old bootloader)
    im = ipod.images[ipod.osos]
    off = ipod.fwoffset + im['devOffset']
    padded = align_up(len(boot), 2048)
    a = off - off % disk.sector
    old = disk.read(a, align_up(off + padded - a, disk.sector))
    new = bytearray(old)
    new[off - a: off - a + padded] = boot + bytes(padded - len(boot))
    disk.write(a, bytes(new))
    dpos = ipod.fw_start + ipod.diroffset + 40 * ipod.osos
    a = dpos - dpos % disk.sector
    sec = bytearray(disk.read(a, disk.sector))
    o = dpos - a
    struct.pack_into('<I', sec, o + 16, len(boot))
    struct.pack_into('<I', sec, o + 24, 0)
    struct.pack_into('<I', sec, o + 28, sum(boot) & 0xffffffff)
    disk.write(a, bytes(sec))
    log('  Rockbox bootloader installed (%s%s)' % (human(len(boot)), ', Apple OS removed' if backup_path else ''))
    disk.flush()
    # 5. check
    log('checking')
    progress(1.0)
    after = Ipod(disk)
    im2 = after.images[after.osos]
    if (im2['len'] != len(boot) or after.fwoffset != ipod.fwoffset or after.data_fs != 'exFAT'
            or after._read(after.fwoffset + im2['devOffset'], len(boot)) != boot):
        raise SetupError('verify: firmware partition / partition table did not read back correctly')
    log('  firmware partition and partition table read back correctly')
    verify_exfat(disk, part_off, tree, up, log)
    disk.finish()
    zf.close()
    log('DONE.  ' + ('Eject the iPod, unplug it and hold MENU+SELECT to restart.' if backup_path else
                    'Eject the drive and put it in the iPod (or unplug the iPod); hold MENU+SELECT to start.'))
    return plan

# --------------------------------------------------------------------------
# window

def gui():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox, simpledialog
    here = os.path.dirname(os.path.abspath(sys.argv[0]))
    root = tk.Tk()
    root.title('iPod exFAT Setup %s' % VERSION)
    root.minsize(760, 600)
    q = queue.Queue()
    st = dict(items=[], busy=False, first=True)
    TITLE = 'iPod exFAT Setup'
    to_logfile('---- iPod exFAT Setup %s started (Python %s)' % (VERSION, sys.version.split()[0]))

    def tk_error(exc, val, tb):
        import traceback
        text = ''.join(traceback.format_exception(exc, val, tb))
        to_logfile(text)
        messagebox.showerror(TITLE, 'Something went wrong:\n\n%s\n\nDetails: %s' % (val, log_path()))
    root.report_callback_exception = tk_error

    frm = ttk.Frame(root, padding=12); frm.pack(fill='both', expand=True)
    frm.columnconfigure(1, weight=1)
    ttk.Label(frm, text='Sets up an iPod Video (5G/5.5G) with a standard exFAT partition and the bcm-rockbox '
              'firmware only -- no Apple OS.  EVERYTHING on the chosen disk is erased.',
              wraplength=720, justify='left').grid(row=0, column=0, columnspan=3, sticky='w', pady=(0, 8))

    mode = tk.StringVar(value='ipod')
    mf = ttk.Frame(frm); mf.grid(row=1, column=0, columnspan=3, sticky='w', pady=(0, 6))
    ttk.Radiobutton(mf, text='Convert a connected iPod (keeps its firmware partition, backs it up)',
                    variable=mode, value='ipod').pack(anchor='w')
    ttk.Radiobutton(mf, text='New / blank drive (SD card, CF card, iPod hard drive in a USB adapter, or an iPod '
                    'in disk mode): build everything', variable=mode, value='new').pack(anchor='w')

    disk_lbl = ttk.Label(frm, text='iPod:'); disk_lbl.grid(row=2, column=0, sticky='w')
    disk_var = tk.StringVar()
    disk_box = ttk.Combobox(frm, textvariable=disk_var, state='readonly')
    disk_box.grid(row=2, column=1, sticky='ew', padx=6)
    rf = ttk.Button(frm, text='Refresh'); rf.grid(row=2, column=2)
    info = ttk.Label(frm, text='', wraplength=720, justify='left', foreground='#555')
    info.grid(row=3, column=0, columnspan=3, sticky='w', pady=(2, 6))

    model_lbl = ttk.Label(frm, text='iPod model:')
    model_var = tk.StringVar(value='iPod 5.5G (Enhanced: 30/80 GB, search in the menu)')
    model_box = ttk.Combobox(frm, textvariable=model_var, state='readonly', values=[
        'iPod 5.5G (Enhanced: 30/80 GB, search in the menu)', 'iPod 5G (original 30/60 GB)'])

    def file_row(r, label, default, types):
        ttk.Label(frm, text=label).grid(row=r, column=0, sticky='w')
        v = tk.StringVar(value=default if os.path.exists(default) else '')
        ttk.Entry(frm, textvariable=v).grid(row=r, column=1, sticky='ew', padx=6, pady=2)
        ttk.Button(frm, text='Browse...', command=lambda: v.set(
            filedialog.askopenfilename(filetypes=types) or v.get())).grid(row=r, column=2)
        return v
    zip_var = file_row(5, 'Rockbox build:', os.path.join(here, 'rockbox.zip'), [('Rockbox build', '*.zip')])
    boot_var = file_row(6, 'Bootloader:', os.path.join(here, 'bootloader-ipodvideo.ipod'),
                        [('iPod bootloader', '*.ipod')])

    bk_lbl = ttk.Label(frm, text='Backup file:'); bk_lbl.grid(row=7, column=0, sticky='w')
    bdir = os.path.join(os.path.expanduser('~'), 'Documents')
    if not os.path.isdir(bdir):
        bdir = os.path.expanduser('~')
    bk_var = tk.StringVar(value=os.path.join(bdir, 'ipod_backup_%s.bin' % time.strftime('%Y%m%d_%H%M')))
    bk_ent = ttk.Entry(frm, textvariable=bk_var); bk_ent.grid(row=7, column=1, sticky='ew', padx=6, pady=2)
    bk_btn = ttk.Button(frm, text='Browse...', command=lambda: bk_var.set(filedialog.asksaveasfilename(
        defaultextension='.bin', initialfile=os.path.basename(bk_var.get())) or bk_var.get()))
    bk_btn.grid(row=7, column=2)

    kd_lbl = ttk.Label(frm, text='Saved .rockbox:')
    kd_var = tk.StringVar()
    kd_ent = ttk.Entry(frm, textvariable=kd_var)
    kd_btn = ttk.Button(frm, text='Browse...', command=lambda: kd_var.set(
        filedialog.askdirectory(title='A saved .rockbox folder (settings, themes, fonts, vc)') or kd_var.get()))

    opt = ttk.Frame(frm); opt.grid(row=9, column=0, columnspan=3, sticky='w', pady=6)
    keep_var = tk.BooleanVar(value=True)
    keep_chk = ttk.Checkbutton(opt, text='Keep my Rockbox settings, themes, fonts and VideoCore files',
                               variable=keep_var)
    keep_chk.pack(side='left')
    ttk.Label(opt, text='   Name:').pack(side='left')
    label_var = tk.StringVar(value='IPOD')
    ttk.Entry(opt, textvariable=label_var, width=12).pack(side='left')
    ttk.Label(opt, text='   Cluster:').pack(side='left')
    cl_var = tk.StringVar(value='128 KB')
    ttk.Combobox(opt, textvariable=cl_var, values=['32 KB', '64 KB', '128 KB', '256 KB'], width=8,
                 state='readonly').pack(side='left')

    bar = ttk.Progressbar(frm, maximum=1000); bar.grid(row=10, column=0, columnspan=3, sticky='ew', pady=6)
    logw = tk.Text(frm, height=13, wrap='word', state='disabled')
    logw.grid(row=11, column=0, columnspan=3, sticky='nsew'); frm.rowconfigure(11, weight=1)
    btns = ttk.Frame(frm); btns.grid(row=12, column=0, columnspan=3, sticky='ew', pady=(8, 0))
    go = ttk.Button(btns, text='Erase iPod and install Rockbox on exFAT'); go.pack(side='right')
    rs = ttk.Button(btns, text='Restore a backup...'); rs.pack(side='left')

    def log(s):
        to_logfile(s)
        q.put(('log', s))
    def prog(f): q.put(('prog', f))

    def refresh(*_):
        try:
            if mode.get() == 'ipod':
                st['items'] = [(n, ip) for n, ip in find_ipods()]
                disk_box['values'] = ['Disk %d: %s, %s' % (n, ip.disk.name, human(ip.disk.size))
                                      for n, ip in st['items']]
                empty = ('No iPod with Apple\'s firmware partition found.  Connect it in Rockbox USB mode (or '
                         'disk mode) and press Refresh -- or, for an empty / unformatted disk, choose '
                         '"New / blank drive" above.')
                if not st['items'] and st['first'] and any(i['ok'] for n, i in list_disks()):
                    st['first'] = False
                    mode.set('new')          # nothing to convert: offer the blank-drive mode
                    return
            else:
                st['items'] = [(n, i) for n, i in list_disks() if i['ok']]
                disk_box['values'] = ['Disk %d: %s, %s, %s%s%s%s' % (
                    n, i['name'], human(i['size']), i['bus'], (' ' + ' '.join(i['letters'])) if i['letters'] else '',
                    '  (empty)' if i['blank'] else '', '  -- INTERNAL DISK' if i['internal'] else '')
                    for n, i in st['items']]
                empty = ('No disk found (the disk Windows runs from is never listed).  Connect the card reader, '
                         'adapter or iPod and press Refresh.')
            st['first'] = False
        except Exception as ex:
            to_logfile('refresh failed: %r' % ex)
            st['items'] = []
            disk_box['values'] = []
            empty = 'Could not list the disks: %s  (Is the tool running as administrator?)' % ex
        if st['items']:
            disk_box.current(0); show()
        else:
            disk_var.set(''); info['text'] = empty
    def show(*_):
        i = disk_box.current()
        if not (0 <= i < len(st['items'])):
            return
        n, it = st['items'][i]
        if mode.get() == 'ipod':
            info['text'] = it.describe()
        else:
            info['text'] = ('%s, %s, %s bus, PC sees %d-byte sectors%s%s.  The whole disk will be erased.%s'
                            % (it['name'], human(it['size']), it['bus'], it['sector'],
                               (', drives ' + ' '.join(it['letters'])) if it['letters'] else '',
                               ', empty (nothing on it)' if it['blank'] else '',
                               '  WARNING: this is an INTERNAL disk of this PC.' if it['internal'] else ''))
    disk_box.bind('<<ComboboxSelected>>', show)

    def set_mode(*_):
        new = mode.get() == 'new'
        disk_lbl['text'] = 'Disk:' if new else 'iPod:'
        go['text'] = 'Erase disk and build iPod layout' if new else 'Erase iPod and install Rockbox on exFAT'
        if new:
            bk_lbl.grid_remove(); bk_ent.grid_remove(); bk_btn.grid_remove(); keep_chk.pack_forget()
            model_lbl.grid(row=4, column=0, sticky='w'); model_box.grid(row=4, column=1, sticky='ew', padx=6, pady=2)
            kd_lbl.grid(row=8, column=0, sticky='w'); kd_ent.grid(row=8, column=1, sticky='ew', padx=6, pady=2)
            kd_btn.grid(row=8, column=2)
        else:
            model_lbl.grid_remove(); model_box.grid_remove(); kd_lbl.grid_remove(); kd_ent.grid_remove()
            kd_btn.grid_remove()
            bk_lbl.grid(); bk_ent.grid(); bk_btn.grid()
            keep_chk.pack(side='left', before=opt.winfo_children()[1])
        refresh()
    mode.trace_add('write', set_mode)

    def pump():
        try:
            while True:
                k, v = q.get_nowait()
                if k == 'log':
                    logw['state'] = 'normal'; logw.insert('end', v + '\n'); logw.see('end'); logw['state'] = 'disabled'
                elif k == 'prog':
                    bar['value'] = int(v * 1000)
                elif k == 'done':
                    st['busy'] = False
                    for b in (go, rs, rf):
                        b['state'] = 'normal'
                    if v:
                        messagebox.showerror(TITLE, v)
                    elif mode.get() == 'new':
                        messagebox.showinfo(TITLE, 'Done.  Eject the drive and put it in the iPod (or unplug the '
                                            'iPod), then hold MENU+SELECT to start it.')
                    else:
                        messagebox.showinfo(TITLE, 'Done.  Eject the iPod, unplug it, and hold MENU+SELECT to '
                                            'restart it.')
        except queue.Empty:
            pass
        root.after(100, pump)

    def run(job):
        st['busy'] = True
        for b in (go, rs, rf):
            b['state'] = 'disabled'
        def worker():
            err = None
            try:
                job()
            except Exception as ex:          # shown to the user
                err = str(ex) or ex.__class__.__name__
                log('ERROR: ' + err)
            q.put(('done', err))
        threading.Thread(target=worker, daemon=True).start()

    def files_ok():
        for v, what in ((zip_var, 'Rockbox build (rockbox.zip)'), (boot_var, 'bootloader (.ipod)')):
            if not os.path.isfile(v.get()):
                messagebox.showwarning(TITLE, 'Choose the %s.' % what); return False
        return True

    def confirm(what):
        ans = simpledialog.askstring('Erase?', 'This erases EVERYTHING on\n\n  %s\n\nType ERASE to continue:' % what,
                                     parent=root)
        return (ans or '').strip().upper() == 'ERASE'

    def do_go():
        i = disk_box.current()
        if not (0 <= i < len(st['items'])):
            messagebox.showwarning(TITLE, 'Nothing selected.'); return
        if not files_ok():
            return
        cl = int(cl_var.get().split()[0]) * 1024
        label = label_var.get() or 'IPOD'
        n, it = st['items'][i]
        if mode.get() == 'new':
            unit = MODELS['5g' if model_var.get().startswith('iPod 5G') else '5.5g']
            kd = kd_var.get().strip() or None
            if kd and not os.path.isdir(kd):
                messagebox.showwarning(TITLE, 'The saved .rockbox folder does not exist.'); return
            if not kd and not messagebox.askyesno(TITLE, 'No saved .rockbox folder chosen: the iPod gets fresh '
                                                  'settings and NO VideoCore files (/.rockbox/vc, needed for '
                                                  'video).  Continue?'):
                return
            if it['internal'] and not messagebox.askyesno(TITLE, 'Disk %d (%s, %s) is connected INSIDE this PC, '
                                                          'not by USB.  Really use it for the iPod?'
                                                          % (n, it['name'], human(it['size'])), icon='warning'):
                return
            if not confirm('%s, %s%s' % (it['name'], human(it['size']),
                                         (' (drives ' + ' '.join(it['letters']) + ')') if it['letters'] else '')):
                return
            def job():
                d = WinDisk(n)
                try:
                    new_drive(d, unit, zip_var.get(), boot_var.get(), keep_dir=kd, label=label, cluster=cl,
                              log=log, progress=prog)
                finally:
                    d.close()
            return run(job)
        keep_dir = None
        if keep_var.get():
            d = WinDisk(n, writable=False)
            try:
                src = keep_source(d, log)
            finally:
                d.close()
            if not src:
                a2 = messagebox.askyesnocancel('Keep settings', 'Windows has no drive letter with a /.rockbox '
                                               'folder for this iPod.\n\nYes: choose a saved .rockbox folder on '
                                               'this PC\nNo: install fresh settings (no VideoCore files)\n'
                                               'Cancel: stop')
                if a2 is None:
                    return
                if a2:
                    keep_dir = filedialog.askdirectory(title='Your saved .rockbox folder')
                    if not keep_dir:
                        return
        if not confirm(it.describe() + '\n(music, videos, playlists ...) and removes the Apple OS'):
            return
        def job():
            d = WinDisk(n)
            try:
                setup(d, zip_var.get(), boot_var.get(), bk_var.get(), keep=keep_var.get(), keep_dir=keep_dir,
                      label=label, cluster=cl, log=log, progress=prog)
            finally:
                d.close()
        run(job)

    def do_restore():
        if mode.get() != 'ipod':
            mode.set('ipod')
        i = disk_box.current()
        if not (0 <= i < len(st['items'])):
            messagebox.showwarning(TITLE, 'No iPod selected (disk mode works: hold SELECT+PLAY while it '
                                   'restarts).'); return
        path = filedialog.askopenfilename(filetypes=[('iPod backup', '*.bin')])
        if not path:
            return
        if not messagebox.askyesno('Restore?', 'Write the partition table and firmware partition from\n%s\n'
                                   'back to the iPod?' % path):
            return
        n = st['items'][i][0]
        def job():
            d = WinDisk(n)
            try:
                restore_backup(d, path, log)
            finally:
                d.close()
        run(job)

    go['command'] = do_go; rs['command'] = do_restore; rf['command'] = refresh
    root.after(100, pump)
    root.after(200, refresh)
    root.mainloop()

# --------------------------------------------------------------------------

def main():
    try:
        return main2()
    except SystemExit:
        raise
    except Exception:
        import traceback
        text = traceback.format_exc()
        to_logfile(text)
        msg = 'iPod exFAT Setup stopped with an error:\n\n%s\nSaved to %s' % (text.strip().splitlines()[-1], log_path())
        if len(sys.argv) == 1:
            try:
                import tkinter as tk
                from tkinter import messagebox
                r = tk.Tk(); r.withdraw(); messagebox.showerror('iPod exFAT Setup', msg); r.destroy()
            except Exception:
                print(msg); input('Press Enter to close')
        else:
            sys.exit(msg)

def main2():
    a = sys.argv[1:]
    def opt(name, default=None):
        return a[a.index(name) + 1] if name in a else default
    if not a:
        if os.name == 'nt' and not is_admin():
            import ctypes as c
            params = ' '.join('"%s"' % x for x in [os.path.abspath(sys.argv[0])] + a)
            exe = sys.executable
            if exe.lower().endswith('python.exe') and os.path.exists(exe[:-10] + 'pythonw.exe'):
                exe = exe[:-10] + 'pythonw.exe'          # no extra console window
            r = c.windll.shell32.ShellExecuteW(None, 'runas', exe, params, None, 1)
            if r <= 32:
                import tkinter as tk
                from tkinter import messagebox
                w = tk.Tk(); w.withdraw()
                messagebox.showerror('iPod exFAT Setup', 'iPod exFAT Setup needs administrator rights to write '
                                     'to disks.  Start it again and answer "Yes" when Windows asks.')
                w.destroy()
            return
        return gui()
    if os.name == 'nt' and ('--list' in a or '--disks' in a) and not is_admin():
        sys.exit('run this from a command prompt opened with "Run as administrator"')
    if '--list' in a:
        for n, ip in find_ipods():
            print('disk %d: %s' % (n, ip.describe()))
        return
    if '--disks' in a:
        for n, i in list_disks():
            print('disk %d: %s, %s, %s, %s%s%s%s' % (n, i['name'], human(i['size']), i['bus'], ' '.join(i['letters']),
                  '  (empty)' if i['blank'] else '', '  INTERNAL' if i['internal'] else '',
                  '' if i['ok'] else '  (never used: Windows runs from it)'))
        return
    if '--image' in a:
        disk = ImageDisk(opt('--image'), int(opt('--sector', '512')))
    elif '--disk' in a:
        if not is_admin():
            sys.exit('run this from a command prompt opened with "Run as administrator"')
        disk = WinDisk(int(opt('--disk')))
    else:
        sys.exit(__doc__)
    try:
        if '--restore' in a:
            if '--yes' not in a:
                sys.exit('add --yes to really write')
            restore_backup(disk, opt('--restore'))
            return
        if '--info' in a:
            print(Ipod(disk).describe()); return
        if '--new' in a:
            if '--yes' not in a:
                sys.exit('add --yes to really erase the whole disk')
            model = opt('--model', '5.5g').lower()
            if model not in MODELS:
                sys.exit('--model must be 5.5g or 5g')
            new_drive(disk, MODELS[model], opt('--rockbox-zip'), opt('--bootloader'), keep_dir=opt('--keep-dir'),
                      label=opt('--label', 'IPOD'), cluster=int(opt('--cluster-kb', '128')) * 1024)
            return
        if '--yes' not in a:
            sys.exit('add --yes to really erase the iPod')
        setup(disk, opt('--rockbox-zip'), opt('--bootloader'),
              opt('--backup', 'ipod_backup_%s.bin' % time.strftime('%Y%m%d_%H%M%S')),
              keep='--no-keep' not in a, keep_dir=opt('--keep-dir'), label=opt('--label', 'IPOD'),
              cluster=int(opt('--cluster-kb', '128')) * 1024,
              progress=(lambda f: None))
    except SetupError as ex:
        sys.exit('ERROR: %s' % ex)
    finally:
        disk.close()

if __name__ == '__main__':
    main()

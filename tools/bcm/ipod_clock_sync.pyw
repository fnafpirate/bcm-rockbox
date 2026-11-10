"""iPod Clock Sync -- sets the clock of an iPod running bcm-rockbox (overlay v74 or later) whenever it is
connected to this computer in Rockbox's USB mode.  No window, no administrator rights.

How: Rockbox watches the data the computer writes over USB; a sector that starts with
"RBCLOCK1 YYYY-MM-DD HH:MM:SS.mmm" sets its clock at that moment.  This helper writes exactly that to
/.rockbox/clocksync.txt on the iPod (flushed at once), a few seconds after the iPod's drive appears.
Started with Windows by iPod exFAT Setup ("Keep the iPod clock right automatically"); or run it yourself.
Python 3 standard library only.  Log: Documents/ipod_clock_sync_log.txt (kept short).
"""
import datetime, os, sys, time

POLL = 3          # seconds between looks for a new drive
SETTLE = 2        # seconds after it appears (Windows finishes mounting)


def log_path():
    d = os.path.join(os.path.expanduser('~'), 'Documents')
    return os.path.join(d if os.path.isdir(d) else os.path.expanduser('~'), 'ipod_clock_sync_log.txt')


def log(text):
    try:
        p = log_path()
        if os.path.isfile(p) and os.path.getsize(p) > 64 * 1024:
            os.replace(p, p + '.old')
        with open(p, 'a') as f:
            f.write('%s  %s\n' % (datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'), text))
    except OSError:
        pass


def drives():
    roots = []
    if os.name == 'nt':
        import ctypes
        mask = ctypes.windll.kernel32.GetLogicalDrives()
        kind = ctypes.windll.kernel32.GetDriveTypeW
        for i in range(2, 26):
            if mask & (1 << i):
                r = '%s:\\' % chr(65 + i)
                if kind(r) in (2, 3):           # removable or fixed (iPods / card readers show as either)
                    roots.append(r)
    else:
        for base in ('/Volumes', '/media', '/run/media'):
            if os.path.isdir(base):
                for d in os.listdir(base):
                    p = os.path.join(base, d)
                    roots.append(p)
                    if os.path.isdir(p) and not os.path.exists(os.path.join(p, '.rockbox')):
                        try:
                            roots += [os.path.join(p, x) for x in os.listdir(p)]
                        except OSError:
                            pass
    out = []
    for r in roots:
        try:
            if os.path.isfile(os.path.join(r, '.rockbox', 'rockbox.ipod')):
                out.append(r)
        except OSError:
            pass
    return out


def sync(root):
    now = datetime.datetime.now()
    text = 'RBCLOCK1 %s.%03d \n' % (now.strftime('%Y-%m-%d %H:%M:%S'), now.microsecond // 1000)
    with open(os.path.join(root, '.rockbox', 'clocksync.txt'), 'w', newline='\n') as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    log('clock of the iPod at %s set to %s' % (root, now.strftime('%Y-%m-%d %H:%M:%S')))


def single_instance():
    if os.name != 'nt':
        return True
    import ctypes
    ctypes.windll.kernel32.CreateMutexW(None, False, 'Local\\iPodClockSync_bcm_rockbox')
    return ctypes.windll.kernel32.GetLastError() != 183      # ERROR_ALREADY_EXISTS


def main():
    if not single_instance():
        return
    once = '--once' in sys.argv
    log('started' + (' (once)' if once else ''))
    done = set()                  # drives synced and still there
    while True:
        now = set(drives())
        done &= now
        for r in sorted(now - done):
            time.sleep(SETTLE)
            try:
                sync(r)
                done.add(r)
            except OSError as ex:     # not ready yet: tried again next time
                log('could not write to %s: %s' % (r, ex))
        if once:
            return
        time.sleep(POLL)


if __name__ == '__main__':
    main()

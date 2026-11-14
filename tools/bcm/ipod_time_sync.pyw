#!/usr/bin/env python3
"""iPod Time Sync 1.1 -- sets the clock of a Rockbox iPod (bcm overlay v105 or later) from this computer.

USB storage gives the iPod no way to ask the computer for the time, so this small helper does it: started with
Windows, it watches for the iPod's drive and, when it appears, writes the computer's local time, name and id to
/.rockbox/timesync.dat.  The iPod catches that write as it happens and sets its clock when the cable comes out
("Clock set from <this computer>").

Pairing is done by the iPod: the first computer that syncs it is remembered in /.rockbox/timesync_pc.txt, and
records from any other computer are ignored.  Delete that file on the iPod to pair a different computer.

Only the iPod you choose is synced (1.1): run it once (double-click), pick your iPod's drive, press "Use this iPod",
then "Start with Windows".  The iPod is recognised by its volume serial number, so it is still found when Windows
gives it another drive letter.  (Formatting the iPod gives it a new serial: choose it again afterwards.)
Python 3 standard library only.

  ipod_time_sync.pyw                 the window: choose the iPod, install / remove, sync now
  ipod_time_sync.pyw --background    what Windows starts at log-on: no window, watches for the chosen iPod
  ipod_time_sync.pyw --sync-now      writes the record to the chosen iPod (if connected) and exits
"""
import os
import sys
import time
import uuid
import random
import socket
import shutil
import datetime
import subprocess
import json

APP = 'iPod Time Sync'
VERSION = '1.1'
RUN_VALUE = 'iPodTimeSync'
RECORD_FILE = os.path.join('.rockbox', 'timesync.dat')
MARKER = os.path.join('.rockbox', 'rockbox.ipod')       # a Rockbox iPod (5G / 5.5G)
POLL_S = 2.0
IS_WIN = sys.platform == 'win32'


# ---------------------------------------------------------------- the record (what the iPod reads)

def build_record(now, pc, machine_id, seq=None):
    """512 bytes: 'RBTSYNC1' and key=value lines, the computer's local time to the millisecond."""
    local = datetime.datetime.fromtimestamp(now)
    tz_min = int(round(local.astimezone().utcoffset().total_seconds() / 60.0))
    seq = seq or '%016x' % random.getrandbits(64)
    clean = lambda s: ''.join(c for c in str(s) if 0x20 <= ord(c) < 0x7f and c not in '=\n\r')[:60] or '?'
    text = ('RBTSYNC1\n'
            'local=%s.%03d\n'
            'utc=%d\n'
            'tz=%d\n'
            'pc=%s\n'
            'id=%s\n'
            'seq=%s\n'
            'from=%s %s\n') % (local.strftime('%Y-%m-%d %H:%M:%S'), local.microsecond // 1000, int(now), tz_min,
                               clean(pc), clean(machine_id), seq, APP, VERSION)
    data = text.encode('ascii')
    return data + b'\n' * (511 - len(data)) + b'\0'


def machine_id():
    """Windows MachineGuid (stays the same until Windows is reinstalled); elsewhere a MAC-based id."""
    if IS_WIN:
        try:
            import winreg
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Microsoft\Cryptography', 0,
                               winreg.KEY_READ | getattr(winreg, 'KEY_WOW64_64KEY', 0))
            v, _ = winreg.QueryValueEx(k, 'MachineGuid')
            winreg.CloseKey(k)
            if v:
                return str(v)
        except OSError:
            pass
    return 'mac-%012x' % uuid.getnode()


def computer_name():
    return os.environ.get('COMPUTERNAME') or socket.gethostname() or '?'


def write_record(root, now=None):
    """Writes the record to the iPod mounted at root and forces it to the device.  Returns the time sent."""
    path = os.path.join(root, RECORD_FILE)
    now = time.time() if now is None else now
    data = build_record(now, computer_name(), machine_id())
    mode = 'r+b' if os.path.exists(path) else 'wb'
    with open(path, mode, buffering=0) as f:
        f.seek(0)
        f.write(data)
        f.truncate(512)
        os.fsync(f.fileno())          # straight to the iPod: it notes when the sector arrives
    return now


# ---------------------------------------------------------------- finding the iPod

def drive_roots():
    if IS_WIN:
        import ctypes
        mask = ctypes.windll.kernel32.GetLogicalDrives()
        return ['%s:\\' % chr(65 + i) for i in range(26) if mask >> i & 1 and i >= 2]   # not A: / B:
    return [os.path.join('/media', d) for d in (os.listdir('/media') if os.path.isdir('/media') else [])]


def find_ipods(roots=None):
    found = []
    for r in (drive_roots() if roots is None else roots):
        try:
            if os.path.isfile(os.path.join(r, MARKER)):
                found.append(r)
        except OSError:
            pass
    return found


def volume_id(root):
    """(serial, label) of the volume at root: the Windows volume serial ("1A2B-3C4D"), which stays the same when
    the drive letter changes; elsewhere the device number.  (None, '') if it cannot be read."""
    if IS_WIN:
        import ctypes
        from ctypes import wintypes
        ser = wintypes.DWORD()
        label = ctypes.create_unicode_buffer(261)
        ok = ctypes.windll.kernel32.GetVolumeInformationW(ctypes.c_wchar_p(root), label, 261, ctypes.byref(ser),
                                                          None, None, None, 0)
        if not ok:
            return None, ''
        return '%04X-%04X' % (ser.value >> 16, ser.value & 0xFFFF), label.value
    try:
        return 'dev-%x' % os.stat(root).st_dev, os.path.basename(root.rstrip('/\\'))
    except OSError:
        return None, ''


def config_path():
    return os.path.join(data_dir(), 'config.json')


def load_choice():
    """the chosen iPod: {'serial', 'label', 'letter'} or None"""
    try:
        with open(config_path(), encoding='utf-8') as f:
            c = json.load(f)
        return c if c.get('serial') else None
    except (OSError, ValueError):
        return None


def save_choice(root):
    serial, label = volume_id(root)
    if not serial:
        raise OSError('cannot read the volume serial of %s' % root)
    c = {'serial': serial, 'label': label, 'letter': root}
    with open(config_path(), 'w', encoding='utf-8') as f:
        json.dump(c, f, indent=1)
    log('iPod chosen: %s (%s, serial %s)' % (root, label or 'no label', serial))
    return c


def chosen_root(choice=None, ipods=None):
    """the drive of the chosen iPod if it is connected now (whatever its letter), else None"""
    choice = load_choice() if choice is None else choice
    if not choice:
        return None
    for r in (find_ipods() if ipods is None else ipods):
        if volume_id(r)[0] == choice['serial']:
            return r
    return None


def describe(root):
    serial, label = volume_id(root)
    return '%s  %s  (serial %s)' % (root, label or '(no label)', serial or '?')


# ---------------------------------------------------------------- background watcher

def data_dir():
    base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    d = os.path.join(base, 'iPodTimeSync')
    os.makedirs(d, exist_ok=True)
    return d


def log(msg):
    try:
        p = os.path.join(data_dir(), 'log.txt')
        lines = []
        if os.path.exists(p):
            with open(p, encoding='utf-8', errors='replace') as f:
                lines = f.read().splitlines()[-199:]
        lines.append('%s  %s' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg))
        with open(p, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
    except OSError:
        pass


def single_instance():
    if not IS_WIN:
        return True
    import ctypes
    ctypes.windll.kernel32.CreateMutexW(None, False, 'Local\\iPodTimeSyncWatcher')
    return ctypes.windll.kernel32.GetLastError() != 183          # ERROR_ALREADY_EXISTS


def watch(stop_after=None):
    """Writes the record once each time the CHOSEN iPod's drive appears (1.1: no other drive is touched).
    A 'quit' file in the data folder stops it.  The choice is read again each time, so a new one applies at once."""
    if not single_instance():
        return
    quit_flag = os.path.join(data_dir(), 'quit')
    if os.path.exists(quit_flag):
        os.remove(quit_flag)
    seen = set()
    log('watching for iPods (%s, id %s)' % (computer_name(), machine_id()))
    t_end = None if stop_after is None else time.time() + stop_after
    while t_end is None or time.time() < t_end:
        if os.path.exists(quit_flag):
            os.remove(quit_flag)
            log('stopped')
            return
        r0 = chosen_root()
        now_ipods = {r0} if r0 else set()
        for r in sorted(now_ipods - seen):
            time.sleep(1.0)                                     # let Windows finish mounting it
            try:
                sent = write_record(r)
                log('%s: time sent (%s)' % (r, datetime.datetime.fromtimestamp(sent).strftime('%Y-%m-%d %H:%M:%S')))
            except OSError as e:
                log('%s: could not write the time: %s' % (r, e))
        seen = now_ipods
        time.sleep(POLL_S)


# ---------------------------------------------------------------- start with Windows

def installed_copy():
    return os.path.join(data_dir(), 'ipod_time_sync.pyw')


def pythonw():
    d = os.path.dirname(sys.executable)
    w = os.path.join(d, 'pythonw.exe')
    return w if os.path.exists(w) else sys.executable


def run_key(write=None, delete=False):
    import winreg
    k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Run', 0,
                       winreg.KEY_READ | winreg.KEY_SET_VALUE)
    try:
        if delete:
            try:
                winreg.DeleteValue(k, RUN_VALUE)
            except FileNotFoundError:
                pass
            return None
        if write is not None:
            winreg.SetValueEx(k, RUN_VALUE, 0, winreg.REG_SZ, write)
            return write
        try:
            return winreg.QueryValueEx(k, RUN_VALUE)[0]
        except FileNotFoundError:
            return None
    finally:
        winreg.CloseKey(k)


def install():
    src = os.path.abspath(__file__)
    dst = installed_copy()
    if os.path.normcase(src) != os.path.normcase(dst):
        shutil.copy2(src, dst)
    cmd = '"%s" "%s" --background' % (pythonw(), dst)
    run_key(write=cmd)
    flags = getattr(subprocess, 'DETACHED_PROCESS', 0) | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
    subprocess.Popen([pythonw(), dst, '--background'], creationflags=flags, close_fds=True)
    log('installed: %s' % cmd)


def uninstall():
    run_key(delete=True)
    with open(os.path.join(data_dir(), 'quit'), 'w') as f:
        f.write('quit\n')
    log('removed from start-up')


# ---------------------------------------------------------------- window

def gui():
    import tkinter as tk
    from tkinter import ttk, messagebox

    root = tk.Tk()
    root.title('%s %s' % (APP, VERSION))
    root.resizable(False, False)
    frm = ttk.Frame(root, padding=14)
    frm.grid()
    ttk.Label(frm, text=APP, font=('Segoe UI', 13, 'bold')).grid(row=0, column=0, columnspan=3, sticky='w')
    ttk.Label(frm, wraplength=470, justify='left', text=(
        'Sets your iPod\'s clock from this computer every time you plug it in.  Needs Rockbox with bcm overlay '
        'v105 or later on the iPod.\n\nOnly the iPod you choose here is synced: pick its drive and press "Use this '
        'iPod".  It is recognised by its volume serial, so a different drive letter later does not matter.\n\n'
        'The iPod itself takes the time only from the computer it is paired with (the first one that syncs it).  '
        'To pair a different computer, delete .rockbox\\timesync_pc.txt on the iPod.')
    ).grid(row=1, column=0, columnspan=3, sticky='w', pady=(4, 10))
    who = ttk.Label(frm, text='This computer: %s' % computer_name())
    who.grid(row=2, column=0, columnspan=3, sticky='w')

    pick = ttk.LabelFrame(frm, text='Your iPod', padding=8)
    pick.grid(row=3, column=0, columnspan=3, sticky='ew', pady=(8, 6))
    drives = []
    drive_var = tk.StringVar()
    combo = ttk.Combobox(pick, textvariable=drive_var, state='readonly', width=52)
    combo.grid(row=0, column=0, sticky='w')
    chosen_lbl = ttk.Label(pick, text='')
    chosen_lbl.grid(row=1, column=0, columnspan=3, sticky='w', pady=(6, 0))
    status = ttk.Label(frm, text='')
    status.grid(row=4, column=0, columnspan=3, sticky='w', pady=(2, 10))

    def list_drives():
        drives[:] = find_ipods()
        names = [describe(r) for r in drives]
        combo['values'] = names
        cur = chosen_root(ipods=drives)
        if cur in drives:
            combo.current(drives.index(cur))
        elif names and combo.current() < 0:
            combo.current(0)
        elif not names:
            drive_var.set('')

    def refresh():
        try:
            on = bool(run_key()) if IS_WIN else False
        except OSError:
            on = False
        c = load_choice()
        if c:
            r = chosen_root(c)
            chosen_lbl.config(text='Chosen: %s (serial %s) -- %s' % (c.get('label') or '(no label)', c['serial'],
                                                                  'connected as %s' % r if r else 'not connected'))
        else:
            chosen_lbl.config(text='Chosen: none yet -- nothing is synced until you choose your iPod')
        status.config(text='Starts with Windows: %s' % ('yes' if on else 'no'))

    def do_choose():
        i = combo.current()
        if i < 0 or i >= len(drives):
            messagebox.showwarning(APP, 'No Rockbox iPod drive to choose.  Plug it in, wait for its drive letter, '
                                        'and press Refresh.')
            return
        try:
            c = save_choice(drives[i])
            messagebox.showinfo(APP, 'Only this iPod will be synced:\n\n%s\n%s (serial %s)'
                                % (drives[i], c.get('label') or '(no label)', c['serial']))
        except OSError as e:
            messagebox.showerror(APP, 'Could not choose it: %s' % e)
        refresh()

    ttk.Button(pick, text='Refresh', command=lambda: (list_drives(), refresh())).grid(row=0, column=1, padx=6)
    ttk.Button(pick, text='Use this iPod', command=do_choose).grid(row=0, column=2)

    def do_install():
        if not load_choice():
            messagebox.showwarning(APP, 'Choose your iPod first (pick its drive and press "Use this iPod").')
            return
        try:
            install()
            messagebox.showinfo(APP, 'Installed.  It runs in the background from now on and starts with Windows.\n\n'
                                     'Plug the iPod in: after you unplug it, it shows "Clock set from %s".'
                                % computer_name())
        except Exception as e:
            messagebox.showerror(APP, 'Could not install: %s' % e)
        refresh()

    def do_remove():
        try:
            uninstall()
            messagebox.showinfo(APP, 'Removed.  It no longer starts with Windows (the running copy stops too).')
        except Exception as e:
            messagebox.showerror(APP, 'Could not remove: %s' % e)
        refresh()

    def do_sync():
        if not load_choice():
            messagebox.showwarning(APP, 'Choose your iPod first (pick its drive and press "Use this iPod").')
            return
        r = chosen_root()
        if not r:
            messagebox.showwarning(APP, 'Your chosen iPod is not connected.  Plug it in and wait for its drive letter.')
            return
        try:
            write_record(r)
            messagebox.showinfo(APP, '%s: time sent.\n\nUnplug the iPod to set its clock.' % r)
        except OSError as e:
            messagebox.showerror(APP, '%s: %s' % (r, e))

    ttk.Button(frm, text='Start with Windows', command=do_install).grid(row=5, column=0, sticky='w')
    ttk.Button(frm, text='Remove', command=do_remove).grid(row=5, column=1, sticky='w', padx=6)
    ttk.Button(frm, text='Sync now', command=do_sync).grid(row=5, column=2, sticky='w')
    list_drives()
    refresh()
    def tick():
        refresh()
        root.after(3000, tick)
    root.after(3000, tick)
    root.mainloop()


def main(argv):
    if '--background' in argv:
        watch()
    elif '--sync-now' in argv:
        r = chosen_root()
        if not r:
            sys.exit('the chosen iPod is not connected (or none chosen: run without arguments to choose it)')
        write_record(r)
        print('%s: time sent' % r)
    elif '--record' in argv:                               # for tests: print the record that would be sent
        sys.stdout.write(build_record(time.time(), computer_name(), machine_id()).rstrip(b'\0\n').decode() + '\n')
    else:
        gui()


if __name__ == '__main__':
    main(sys.argv[1:])

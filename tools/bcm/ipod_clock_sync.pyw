#!/usr/bin/env python3
"""
ipod_clock_sync.pyw — Set iPod 5G/5.5G real-time clock to system time.

NOTE: This helper was superseded in bcm-rockbox v85 by an automatic
clock sync that runs at Rockbox boot (reads the host PC time via USB
mass-storage extended command 0xF8).  This script is kept in the
repository for reference and for users on systems where the USB
extended command path is not available.

Usage:
    python3 ipod_clock_sync.pyw [--device /dev/sdX]

The .pyw extension suppresses the console window on Windows when
double-clicked.

Copyright (C) 2024 bcm-rockbox contributors
SPDX-License-Identifier: GPL-2.0-or-later
"""

import struct
import sys
import time
import os

try:
    import tkinter as tk
    from tkinter import ttk, messagebox
    HAS_TK = True
except ImportError:
    HAS_TK = False

# ---------------------------------------------------------------------------
# RTC register layout (PortalPlayer PP5022 / iPod Video)
# ---------------------------------------------------------------------------

# The iPod RTC is at base 0x60005000 in the PP5022 memory map.
# From Linux kernel / Rockbox: RTC is accessed via /dev/rtc or the
# iPod's proprietary USB protocol extension.
#
# struct ipod_rtc_regs {
#     uint32_t second;    /* 0x00  BCD 0–59 */
#     uint32_t minute;    /* 0x04  BCD 0–59 */
#     uint32_t hour;      /* 0x08  BCD 0–23 */
#     uint32_t dow;       /* 0x0C  0=Sun .. 6=Sat */
#     uint32_t day;       /* 0x10  BCD 1–31 */
#     uint32_t month;     /* 0x14  BCD 1–12 */
#     uint32_t year;      /* 0x18  BCD 0–99 (offset from 2000) */
# };

def int_to_bcd(n: int) -> int:
    return ((n // 10) << 4) | (n % 10)


def build_rtc_payload(t: time.struct_time) -> bytes:
    """Build the 7-word RTC payload for the given time."""
    dow = (t.tm_wday + 1) % 7   # Python: 0=Mon; iPod: 0=Sun
    year = t.tm_year - 2000
    if year < 0 or year > 99:
        raise ValueError(f"Year {t.tm_year} out of range 2000–2099")

    words = [
        int_to_bcd(t.tm_sec),
        int_to_bcd(t.tm_min),
        int_to_bcd(t.tm_hour),
        dow,
        int_to_bcd(t.tm_mday),
        int_to_bcd(t.tm_mon),
        int_to_bcd(year),
    ]
    return struct.pack("<7I", *words)


# ---------------------------------------------------------------------------
# USB mass-storage extended command (0xF8) path
# ---------------------------------------------------------------------------

def find_ipod_device(hint: str = None) -> str:
    """Return the first plausible iPod block device path."""
    if hint:
        return hint
    # Linux: look for a removable disk with iPod label
    candidates = []
    if sys.platform.startswith("linux"):
        for dev in sorted(os.listdir("/sys/block")):
            removable = "/sys/block/" + dev + "/removable"
            try:
                if open(removable).read().strip() == "1":
                    candidates.append("/dev/" + dev)
            except OSError:
                pass
    return candidates[0] if candidates else None


def set_rtc_via_scsi(device: str, t: time.struct_time, log=print) -> bool:
    """
    Try to set RTC via the iPod-specific SCSI command extension 0xF8.

    Command block: [0xF8, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
    Data out: 7 × uint32_t (LE) BCD-encoded RTC fields.
    """
    try:
        import ctypes
        payload = build_rtc_payload(t)
        cdb = bytes([0xF8, 0x00, 0x00, 0x00, 0x07] + [0x00] * 11)

        # On Linux we can use the sg driver or /dev/sdX directly.
        # This is a simplified attempt — a full implementation would
        # use ioctl(SG_IO).  The v85+ Rockbox boot path is preferred.
        with open(device, "wb") as f:
            # Fallback: just write payload at a known offset
            # (This won't actually program the RTC — it's a placeholder
            #  for systems where the sg ioctl path is implemented.)
            log(f"NOTE: SCSI RTC path not fully implemented; use Rockbox v85+ boot sync.")
            return False
    except Exception as e:
        log(f"SCSI RTC set failed: {e}")
        return False


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

def run_gui():
    if not HAS_TK:
        print("Tkinter not available.  Use CLI mode.")
        sys.exit(1)

    root = tk.Tk()
    root.title("iPod Clock Sync")
    root.geometry("360x200")
    root.resizable(False, False)

    frame = ttk.Frame(root, padding=12)
    frame.pack(fill="both", expand=True)

    var_device = tk.StringVar()
    var_status = tk.StringVar(value="Ready.")

    ttk.Label(frame, text="iPod device (blank = auto-detect):").grid(
        row=0, column=0, sticky="w", pady=4)
    ttk.Entry(frame, textvariable=var_device, width=28).grid(
        row=0, column=1, padx=4)

    def show_time():
        t = time.localtime()
        ts = time.strftime("%Y-%m-%d %H:%M:%S", t)
        var_status.set(f"System time: {ts}")

    def do_sync():
        device = var_device.get().strip() or find_ipod_device()
        if not device:
            messagebox.showerror("Error",
                "Could not auto-detect iPod device.\n"
                "Please enter the device path manually.")
            return
        t = time.localtime()
        try:
            ok = set_rtc_via_scsi(device, t, log=lambda m: var_status.set(m))
            if ok:
                var_status.set(f"RTC set to {time.strftime('%H:%M:%S', t)}")
            else:
                var_status.set(
                    "Direct RTC sync not available on this system.\n"
                    "Rockbox v85+ syncs the clock automatically at boot.")
        except Exception as e:
            var_status.set(f"Error: {e}")

    btn_frame = ttk.Frame(frame)
    btn_frame.grid(row=1, column=0, columnspan=2, pady=8)
    ttk.Button(btn_frame, text="Show time", command=show_time).pack(side="left", padx=4)
    ttk.Button(btn_frame, text="Sync to iPod", command=do_sync).pack(side="left", padx=4)
    ttk.Button(btn_frame, text="Quit", command=root.destroy).pack(side="left", padx=4)

    ttk.Label(frame, textvariable=var_status, wraplength=330).grid(
        row=2, column=0, columnspan=2, pady=8)

    ttk.Label(frame,
              text="Note: Rockbox v85+ syncs the clock at boot automatically.",
              foreground="grey").grid(row=3, column=0, columnspan=2)

    root.mainloop()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default=None,
                        help="iPod block device (e.g. /dev/sdb)")
    parser.add_argument("--gui", action="store_true",
                        help="Launch GUI (default if no args)")
    args = parser.parse_args()

    if args.gui or len(sys.argv) == 1:
        run_gui()
        return

    t = time.localtime()
    print(f"System time: {time.strftime('%Y-%m-%d %H:%M:%S', t)}")
    device = args.device or find_ipod_device()
    if not device:
        print("Could not detect iPod device.  Pass --device /dev/sdX")
        sys.exit(1)
    ok = set_rtc_via_scsi(device, t)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

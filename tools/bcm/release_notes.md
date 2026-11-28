## bcm-rockbox v145

First public release.  Rockbox for the iPod Video (5G / 5.5G) with hardware video playback on the iPod's VideoCore
chip, TV out and exFAT drives of any size.

**New in v145**
* iPod exFAT Setup 1.16 works from anywhere: its files live in `C:\bcm rockbox` (Mac / Linux: `~/bcm rockbox`), the
  .exe carries the Rockbox build and bootloader inside, and it checks this page for updates.
* *Update Rockbox on iPod*: installs a newer release on a Rockbox iPod without formatting (settings, music and video
  files stay).
* Everything from v144: TV screensaver, display rescue (MENU three times), sleep fade, exact Opus seeking, Battery Load
  Check, series view, faster start, exFAT "dirty" mark only while writing, and more.

**Which file do I need?**  Windows 10 / 11: `iPod-exFAT-Setup-win64.exe` -- Windows 7 / 8: `-win32.exe` -- Mac:
`-mac-arm64.zip` (Apple silicon) or `-mac-intel.zip` -- anything with Python 3.8+: `ipod_exfat_setup.py`.

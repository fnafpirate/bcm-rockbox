# bcm-rockbox

**An unofficial, experimental fork of [Rockbox](https://www.rockbox.org) for the iPod Video (5th generation, 5G and
5.5G), that plays video on the iPod's own video chip and runs on big exFAT drives (iFlash, SD and CF cards, up to
multi-terabyte).**

> **Made with AI.** Every line of code that this fork adds or changes was written by an AI model (Anthropic's
> Claude), directed, tested on real hardware and published by one person as a hobby / passion project.  It is shared
> as-is, for free, in case it is useful to other iPod Video owners.  It is not a product, it has no support team, and
> it is **not affiliated with, endorsed by or connected to Apple, Broadcom or the Rockbox project.**  Read
> [DISCLAIMER.md](DISCLAIMER.md) before you install it.

Keywords: iPod Video, iPod 5G, iPod 5.5G, iPod 5th generation, MA002, MA003, MA146, MA147, MA444, MA446, MA448, MA450, Rockbox,
exFAT, iFlash, iFlash Quad, SD card, VideoCore, BCM2722, H.264 video, TV out, composite, custom firmware.

---

## What it does

Stock Rockbox on the iPod Video plays video in software on the iPod's ARM processor (small MPEG files only).  The
iPod Video also has a Broadcom BCM2722 "VideoCore" chip that Apple's own firmware uses for H.264 / MPEG-4 video and
TV output.  This fork talks to that chip the way Apple's firmware does, so Rockbox can:

* play **H.264 / MPEG-4 .mp4 / .m4v videos** in hardware, full screen, with seeking, resume and subtitles,
  including High / Main profile files with CABAC (converted to a form the chip accepts on the fly);
* send video and its own menus to a **TV** (composite, PAL or NTSC);
* run on **exFAT** drives of any size (FAT32 still works), with a Windows tool that formats and sets up the iPod in
  one step;
* plus many smaller things: a steadier battery reading, faster start-up, a TV screensaver, a display rescue, docks
  and remotes controlling video, and more (see the release notes).

Supported: **iPod Video 5G and 5.5G (30 / 60 / 80 GB models, any storage mod).**  Nothing else -- not the iPod
Classic 6th / 7th gen, nano or mini.

**New here? Read the [User Guide](GUIDE.md)**: installing, the video player's buttons, video settings, TV out,
subtitles, the battery learner, updating and fixing problems.

## How it works (short version)

1. The iPod's boot ROM starts the **Rockbox bootloader** (built from this repository).
2. The bootloader loads **Rockbox** from `/.rockbox/rockbox.ipod` on the iPod's drive (exFAT or FAT32).
3. For video, Rockbox starts the iPod's video chip (the VideoCore) with **Apple's VideoCore firmware files**.  These
   are Apple's, so they are **not** in this repository or its releases.  They come from Apple's iPod software update
   (an `.ipsw` file), which the setup tool fetches on your computer and copies the needed files from to your iPod.
4. The setup tool finds that `.ipsw` through [The Apple Wiki](https://theapplewiki.com/wiki/Firmware/iPod), a
   community wiki that lists every iPod firmware file and where to download it.  It is **not** an Apple website.
   The tool only keeps the file if its SHA-1 checksum matches Apple's original, so a changed or damaged file is never
   used.
5. When you play a video, Rockbox reads the file, hands the compressed picture to the VideoCore (which decodes it in
   hardware and draws it on the iPod's screen or the TV), and the sound plays alongside it.

## Install

1. Download the newest release from the **[Releases](../../releases)** page:
   * Windows 10 / 11: `iPod-exFAT-Setup-win64.exe`
   * Windows 7 / 8 / 8.1: `iPod-exFAT-Setup-win32.exe`
   * Linux (Python 3.8+ with Tkinter, run with `sudo`): `ipod_exfat_setup.py`
   * Mac is not supported.
2. **Back up your music.  Formatting erases the iPod.**
3. Connect the iPod and run iPod exFAT Setup.  (If the computer does not see it: disk mode -- hold MENU + SELECT
   until it restarts, then at once SELECT + PLAY until "Disk Mode" shows.)
4. Choose the iPod, press *Download* next to "iPod firmware (.ipsw)" (or pick an .ipsw you already have), and start.

Step by step, with pictures of what to expect: the [User Guide](GUIDE.md).

The program keeps its files in `C:\bcm rockbox` (Linux: `~/bcm rockbox`) and, while it is open, checks this page for
updates.
To update later without formatting: connect the iPod in Rockbox's USB mode and press *Update Rockbox on iPod*.

Windows may show "Windows protected your PC" because the program is new and not code-signed -- choose *More info*
then *Run anyway*.  If you would rather not run an .exe, run `ipod_exfat_setup.py` with Python from python.org; it is
the same program.

## Apple's firmware (.ipsw)

The video files come from Apple's iPod software update for your model:

| iPod | File | SHA-1 |
|---|---|---|
| 5.5G (Enhanced) | `iPod_25.1.3.ipsw` | `0f2dd984c5d56475bdf3c14b4bc7541b799f2c85` |
| 5G | `iPod_20.1.3.ipsw` | `bf041893dbf1e0ad965bafb9a6eae0be276e164a` |

The setup tool finds the download through [The Apple Wiki](https://theapplewiki.com/wiki/Firmware/iPod) (a community
wiki, not Apple) and only uses the file if its SHA-1 matches the table above.  Use it only with your own iPod.

## Build it yourself

Standard Rockbox build for target `ipodvideo` (see `docs/README` and the Rockbox wiki): build the ARM toolchain with
`tools/rockboxdev.sh`, then `mkdir build && cd build && ../tools/configure --target=ipodvideo --type=n && make && make zip`.
The GitHub workflows in `.github/workflows/` do exactly this.

## License

Rockbox and this fork are free software under the **GNU General Public License, version 2 or later** (see
[LICENSE](LICENSE), `docs/COPYING` and `docs/LICENSES` for code Rockbox imported under other licenses).  The full
source of every release is this repository at the release's tag.  Public releases are numbered v1, v2, ...

## Credits

All of Rockbox -- the operating system, codecs, drivers and the iPod port -- is the work of the
[Rockbox project](https://www.rockbox.org) and its contributors (`docs/CREDITS`).  This fork only adds to it.
Please report problems with this fork **here**, not to the Rockbox project.

iPod is a trademark of Apple Inc., registered in the U.S. and other countries.  VideoCore is a trademark of
Broadcom.  Rockbox is the name of the original project; it is used here only to say where this fork comes from.

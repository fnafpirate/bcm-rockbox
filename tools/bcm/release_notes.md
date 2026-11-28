## bcm-rockbox v1

The first public release.  Rockbox for the iPod Video (5G / 5.5G) that plays video on the iPod's own video chip,
sends video to a TV, and runs on exFAT drives of any size.

**What's in it**
* Hardware H.264 video (.mp4 / .m4v) up to 640x480, including Main / High profile (CABAC) files, with
  seeking, subtitles (.srt / .vtt / .ass next to the video), sleep and continue-where-you-left-off.
* TV out (composite, NTSC / PAL), the picture moved between the TV and the iPod while it plays, a TV screensaver.
* exFAT support for big iFlash / SD / CF cards, and **iPod exFAT Setup**: formats and sets up the iPod in one step,
  fetches Apple's firmware file for the video chip for you, and updates Rockbox later without formatting.
* A steadier battery reading that can learn your battery (Learn Battery), dock and remote control of videos, and
  more -- see the [feature list](../../blob/HEAD/GUIDE.md#features-what-stock-rockbox-doesnt-do) and the
  [User Guide](../../blob/HEAD/GUIDE.md).

**Which file do I need?**  Windows 10 / 11: `iPod-exFAT-Setup-win64.exe` -- Windows 7 / 8: `iPod-exFAT-Setup-win32.exe`
-- Linux: `ipod_exfat_setup.py` (Python 3.8+, run with sudo).  Mac is not supported.

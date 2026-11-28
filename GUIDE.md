# bcm-rockbox User Guide

Everything you need to install bcm-rockbox on an iPod Video (5G / 5.5G), play videos, use TV out, look after the
battery, and fix the usual problems.  Rockbox's own music player, menus and themes work as in normal Rockbox; the
[Rockbox manual](https://www.rockbox.org/manual.shtml) covers them.  This guide covers what bcm-rockbox adds.

> bcm-rockbox is an unofficial hobby project written with AI, provided as-is.  Read [DISCLAIMER.md](DISCLAIMER.md).

**Contents:** [1. Install](#1-install) · [2. Putting videos on it](#2-putting-videos-on-it) ·
[3. Watching a video](#3-watching-a-video) · [4. Video settings](#4-video-settings) · [5. TV out](#5-tv-out) ·
[6. Subtitles](#6-subtitles) · [7. Battery](#7-battery) · [8. Docks, remotes, Bluetooth](#8-docks-remotes-and-bluetooth-transmitters) ·
[9. Storage and USB](#9-storage-and-usb) · [10. Updating](#10-updating) · [11. Problems](#11-when-something-goes-wrong) ·
[12. How it works](#12-how-it-works-for-the-curious)

---

## 1. Install

**You need:** an iPod Video 5G or 5.5G (any storage: original hard drive, iFlash, SD or CF card), a Windows PC
(7 or newer) or a Linux PC, and a USB cable.  Mac is not supported.

**Formatting erases everything on the iPod.  Copy your music and videos somewhere else first.**

1. Download **iPod exFAT Setup** from the [Releases](../../releases) page: `iPod-exFAT-Setup-win64.exe` for Windows
   10 / 11, `iPod-exFAT-Setup-win32.exe` for Windows 7 / 8, or `ipod_exfat_setup.py` for Linux (`sudo python3
   ipod_exfat_setup.py`).  The .exe already contains Rockbox and the bootloader; nothing else to download.
2. Run it.  Windows asks for administrator rights (it writes to a disk): answer **Yes**.  If Windows says
   *"Windows protected your PC"*, click **More info** then **Run anyway** (the program is not code-signed).
3. Connect the iPod.  If the program does not list it, put the iPod in **disk mode**: hold **MENU + SELECT** until it
   restarts (about 6 s), then at once hold **SELECT + PLAY** until "Disk Mode" appears.
4. Pick what to do:
   * **Convert a connected iPod**: an iPod that already has Rockbox or Apple's software.  Your Rockbox settings are
     kept.
   * **New / blank drive**: a new card or an empty drive (in a USB adapter or an iPod in disk mode).
   * **Move to a bigger card / drive**: copies everything from an old card to a new one and fills the new one.
5. Next to **iPod firmware (.ipsw)** press **Download**.  This fetches Apple's software update for your iPod model
   (about 6.5 MB), which holds the video chip's files.  It is found through The Apple Wiki, a community wiki that
   lists Apple's firmware files (it is not an Apple website); the program checks the file's SHA-1 checksum, so only
   Apple's original file is used.  Already have `iPod_25.1.3.ipsw` (5.5G) or `iPod_20.1.3.ipsw` (5G)?  Use **Browse**.
6. Press **Erase iPod and install Rockbox on exFAT**, type `ERASE`, and wait.
7. Unplug the iPod.  It starts Rockbox.

The program keeps its files (the Rockbox build, downloaded firmware, backups) in `C:\bcm rockbox`
(Linux: `~/bcm rockbox`); **Open my files folder** opens it.

**5G or 5.5G?**  The 5.5G ("Enhanced") came in 30 GB and 80 GB and has a search option in its Music menu; the 5G came
in 30 GB and 60 GB.  The program usually tells from the iPod itself and asks if it can't.

---

## 2. Putting videos on it

Connect the iPod while Rockbox runs: it appears as a normal drive on the PC.  Copy videos anywhere; folders named
**Videos**, **TV Shows** or **Movies** get extra features (faster starts, the TV File Browser, episode lists).

**What plays:**

| | Works | Notes |
|---|---|---|
| Container | `.mp4`, `.m4v` | |
| Video | H.264 (Baseline, Main or High profile) | up to 640x480, about 30 frames a second |
| Sound | AAC | stereo |
| Size | any (exFAT has no 4 GB file limit) | |

A good setting for HandBrake or ffmpeg: H.264, 640x480 (or 320x240 to save space), 1-2.5 Mbit/s, AAC 128-160 kbit/s.
Main / High profile files (CABAC) play too: the iPod converts them on the fly, which uses a little more battery than
Baseline.

TV shows: put each show in its own folder inside a folder named **TV Shows** (`/Videos/TV Shows/Some Show/S01E03
Title.mp4`; `Season 2` subfolders, `1x03` and `05 - Title` names work too).  A dock's or remote's next / previous
then goes through the episodes in order.

---

## 3. Watching a video

Pick a video in the file browser.  It starts in about a second (faster in a video folder with *Keep VideoCore Ready*).

| Button | Short press | Hold |
|---|---|---|
| **PLAY** | play / pause | 1.5 s: sleep (it continues from the same spot when you wake it) |
| **MENU** | stop, back to the files | 1 s: move the picture between the TV and the iPod screen |
| **>>\|** / **\|<<** | jump 10 s forward / back | fast-forward / rewind; let go to play from there |
| **SELECT** | show / hide the controls (twice quickly: the clock and the end time) | with subtitles: 0.6 s = next subtitle track / off |
| **Wheel** | volume | |

* **Leaving with MENU** starts the video from the beginning next time.  **Sleep** (hold PLAY, the sleep timer, or
  pausing until the screen goes off) keeps the spot: wake the iPod and it continues.  After about 30 minutes asleep
  it powers off and still continues from the same spot at the next start.
* **Sleep timer**: the sound fades out over 3 seconds, then it pauses and sleeps.
* **Headphones pulled out**: the video pauses (Settings > Playback Settings > Pause on Headphone Unplug).
* Docks and remotes (including Apple's) control the video: play / pause, skip, volume.

---

## 4. Video settings

**Settings > General Settings > Video**

| Setting | Choices | What it does |
|---|---|---|
| **Video Out** | menu | TV out settings (see [5. TV out](#5-tv-out)) |
| **Subtitles** | menu | subtitles on / off, size, style (see [6](#6-subtitles)) |
| **Video Buffering** | Low-Power / Balanced / Performance / **Adaptive** (default) | how much of the video is read ahead into memory.  *Low-Power*: about 3 minutes at a time, the drive wakes the fewest times (best battery).  *Balanced*: about 1 minute.  *Performance*: about 2 minutes, kept fuller, best for lots of seeking.  *Adaptive*: picks by itself from the card's speed and how you watch. |
| **Keep VideoCore Ready** | Off / **In Video Folders** (default) | in folders named Videos, TV Shows or Movies the video chip is started while you browse, so videos start almost at once.  Off saves a little battery while browsing. |
| **Dock Notifications** | On / Off | short "Dock connected" style messages |
| **Dock Line-Out Profile** | On / Off | the iPod keeps separate sound settings (volume, EQ, bass, treble, balance, crossfeed) for docked and for headphones, and switches between them when you dock or undock |

**Settings > Sound Settings** adds **Start Volume** (Off / *50 % When Playback Starts* / *50 % Every Track*: protects
your ears from a volume left high), **Dock Line Out** (Normal / Bluetooth: Bluetooth suits transmitters on the dock
connector) and **Line Out Gain** (0-6 dB more level on the line out).

---

## 5. TV out

You need a composite video cable: Apple's iPod AV cable (3.5 mm jack: yellow video, red / white sound) or a dock
with a video output.  TV out is standard definition (NTSC or PAL); HD is not possible on this iPod.

**Settings > General Settings > Video > Video Out**

| Setting | Choices | What it does |
|---|---|---|
| **Video Out** | On / Ask / **Off** / Auto | *On*: videos always play on the TV.  *Ask*: asks each time a video starts.  *Auto*: the TV when the iPod sits in a dock with video out, otherwise the iPod screen.  The iPod can't tell an AV cable from headphones, so with the cable use *On* or *Ask*. |
| **TV File Browser** | On / Off | with Video Out On (or Auto on a TV dock), opening a video folder shows the file list on the TV in big type; the iPod screen shows a small animation.  Use the wheel and buttons as usual. |
| **TV Signal** | NTSC / PAL | NTSC for North America and Japan, PAL for most of Europe, Australia and elsewhere.  A black-and-white or rolling picture means the wrong one. |
| **Widescreen TV** | On / Off | On for a 16:9 TV |
| **TV Picture** | Letterbox / Fill | Letterbox shows the whole picture with bars; Fill fills the screen and crops the edges |

* During a video, **hold MENU for 1 second** to move the picture between the TV and the iPod screen.
* While the TV shows a video, the iPod's own screen shows `/.rockbox/tv.gif` (any GIF you like; the setup tool's
  **Extras** can install one).
* **TV screensaver**: put a short looping video called `tv_saver.mp4` in `/.rockbox`.  A video paused on the TV for
  2 minutes then plays it until you press a button, and your video comes back at the same frame.
* In the TV File Browser, press **MENU three times quickly** to go back to the iPod's screen (until the next restart).

---

## 6. Subtitles

Put the subtitle file next to the video with the same name: `Movie.mp4` + `Movie.srt` (or `Movie.en.srt`,
`.vtt`, `.ass`, `.ssa`).  Subtitle tracks inside the MP4 (as HandBrake or iTunes write them) work too; files next
to the video come first.  On a TV the subtitles are drawn on the TV, larger.

**Settings > General Settings > Video > Subtitles**: **Subtitles** On / Off, **Subtitle Size** Small / Medium / Large,
**Subtitle Style** Outline / Box.  During a video, **hold SELECT** to switch to the next subtitle track or turn
them off.

---

## 7. Battery

**Settings > General Settings > System > Battery**

| Setting | What it does |
|---|---|
| **Battery Capacity** | your battery's size in mAh (set it if you fitted a bigger battery) |
| **Battery Reading** | *Balanced* (default), *Smooth* or *Exact*.  Smooth and Balanced keep the percentage steady (it does not jump up and down when the screen or a video starts); Exact shows the raw reading. |
| **Learn Battery** | measures your own battery once, so the percentage is right for it (see below) |

**Learn Battery** -- do it once, and again every few months as the battery ages:

1. Charge the iPod to full (the run needs a full battery to start).
2. Unplug it, choose **Learn Battery**, and confirm.
3. Leave it alone until it switches itself off.  That takes many hours (often 10-20): the screen stays on at full
   brightness with a moving picture, so the battery is used steadily.  Don't use the iPod meanwhile; plugging it in
   stops the run, and MENU asks whether to stop.
4. The next start uses what it learned.  If the run was interrupted, the iPod offers to carry on at the next start.

Battery tips for video: *Video Buffering = Low-Power* and a lower screen brightness help most; Baseline-profile
H.264 files use a little less power than Main / High; the screen off on the TV saves the most.

---

## 8. Docks, remotes and Bluetooth transmitters

* Docks and remotes work for music and video (play / pause, skip, volume).  Car stereos and docks with a screen get
  the album art and track details.
* Bluetooth transmitters on the dock connector: set **Settings > Sound Settings > Dock Line Out** to *Bluetooth*, and use
  **Line Out Gain** if it is too quiet.

---

## 9. Storage and USB

* **exFAT**: big cards and files over 4 GB work; the drive opens on any Windows, Mac or Linux PC.
* **Settings > General Settings > System > Disk > Optimize Solid State**: for iFlash / SD / CF (no spinning hard
  drive).  It is switched on by itself at the first start when the iPod finds flash storage.
* **Settings > General Settings > System > USB**: **USB Write Speed** (Stock / Fast) and **USB Read-Ahead** can make
  copying to and from the PC faster.
* **iPod Defrag** (in the releases): rejoins files that were split into pieces, and **Check & fix** repairs the
  "drive needs to be checked" mark after the iPod lost power while writing.

---

## 10. Updating

Open **iPod exFAT Setup**.  With **Check when opened** ticked it looks for a newer version when it starts (it never
runs in the background), or press **Check for updates**.

* **A newer iPod exFAT Setup**: it shows what changed and asks; **Yes** downloads it, checks it and restarts.
* **A newer Rockbox**: connect the iPod while Rockbox runs (it shows as a drive) and press **Update Rockbox on
  iPod**.  Nothing is formatted: settings, music, videos and the video chip's files stay.  Then eject the iPod and
  restart it (hold MENU + SELECT).

---

## 11. When something goes wrong

| Problem | What to do |
|---|---|
| The iPod is frozen | hold **MENU + SELECT** about 6 s: it restarts |
| White or frozen screen in the menus | press **MENU three times quickly**: the screen is restarted |
| Videos don't play ("files missing" or a black screen) | the video chip's files are missing: in iPod exFAT Setup press **Copy video files from .ipsw** with the iPod connected |
| The PC says the drive needs checking | iPod Defrag > **Check & fix**, or Windows' own *Check* on the drive |
| The PC does not see the iPod | disk mode (see [Install](#1-install), step 3) |
| A video stutters | try *Video Buffering = Performance*; very high bit rates (over about 2.5 Mbit/s) are too much for the chip |
| No picture on the TV | check *Video Out*, try the other *TV Signal*, and the cable (Apple's AV cable has video on the yellow plug) |
| Need to report a problem | turn on **Settings > General Settings > System > Debug Log**, repeat the problem, and attach `/bcm_host_log.txt` from the iPod to an issue on GitHub |

Going back to Apple's software: restore the iPod with iTunes / Finder in disk mode (this erases it).

---

## 12. How it works (for the curious)

* The iPod's boot ROM starts the **Rockbox bootloader**, which loads **Rockbox** from `/.rockbox/rockbox.ipod`.
* The iPod Video has a second chip just for video, the **Broadcom VideoCore**.  Apple's software used it for video and
  TV out.  bcm-rockbox talks to it the same way, with Apple's own VideoCore files (kept in `/.rockbox/VideoCore`,
  copied there from Apple's `.ipsw` by the setup tool).
* When you play a video, Rockbox reads the file from the drive into memory, hands the compressed picture to the
  VideoCore (which decodes it and draws it on the screen or the TV) and keeps picture and sound in step.  That is why
  video uses little battery: the main processor mostly waits.
* **exFAT** support lets Rockbox (and the bootloader) use cards of any size.  Files the setup tool copies are written
  in one piece, which keeps reading fast.
* Files bcm-rockbox keeps in `/.rockbox`: `VideoCore/` (Apple's video files), `bcm_video_ui.bin` (the video
  controls' pictures), `bcm_resume.txt` (where to continue), `tv.gif`, `tv_saver.mp4`, `bcm-version.txt` (the release
  installed).  Deleting `VideoCore/` stops video until the files are copied again.

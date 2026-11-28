# Disclaimer

Please read this before installing bcm-rockbox or running its tools.

## A hobby project, written by AI

bcm-rockbox is a personal passion project.  The changes it makes to Rockbox, and the tools in `tools/bcm/`, were
written by an AI model (Anthropic's Claude) at the direction of the project owner, who tests them on his own iPod and
publishes them for free.  AI-written code can contain mistakes that a human expert would not make.  Nobody is paid to
maintain it and there is no promise of fixes, updates or answers.

## No warranty

The software is provided **"as is", without warranty of any kind**, express or implied, including but not limited
to the warranties of merchantability, fitness for a particular purpose and non-infringement (see sections 11 and 12
of the GNU General Public License, version 2, in `LICENSE`).  In no event shall the authors be liable for any claim,
damages or other liability arising from the use of the software.

In plain words:

* **Formatting erases everything on the chosen drive.**  Choosing the wrong drive erases that drive.  Keep backups.
* Custom firmware can crash, drain the battery, corrupt files or leave the iPod needing a restore.  An iPod Video can
  always be restored with iTunes / Finder in disk mode, but your files may be lost.
* You install and use it at your own risk.

## Not affiliated

This project is **not affiliated with, authorised, endorsed or sponsored by Apple Inc., Broadcom Inc. or the Rockbox
project.**  "iPod" is a trademark of Apple Inc.; "VideoCore" is a trademark of Broadcom.  These names are used only to
describe which hardware the software works with.  "Rockbox" names the open-source project this is a fork of; this
fork is not an official Rockbox release, and problems with it should not be reported to the Rockbox project.

## Apple's software

This repository and its releases contain **no Apple software**.  The iPod's video chip needs Apple's VideoCore
firmware files, which are part of Apple's iPod software update (an `.ipsw` file).  The setup tool downloads that file
on the user's computer through the firmware list on The Apple Wiki (a community wiki that is not run by Apple), checks
it against the SHA-1 checksum of Apple's original, and copies the needed files to the user's own iPod.  Apple's software remains subject to Apple's
license terms; use it only with an iPod you own.

The way Rockbox talks to the VideoCore was worked out by studying how the hardware behaves, so that independently
written software can work with it.  To the maintainer's knowledge, no code or data copied from Apple's firmware is in
this repository.  If you believe something here infringes your rights, open an issue or contact the maintainer
through GitHub and it will be looked at and removed if needed.

## License

Rockbox and this fork: GNU General Public License, version 2 or (at your option) any later version.  The complete
corresponding source for every release is this repository at the release's tag, and GitHub attaches it to every
release as "Source code".

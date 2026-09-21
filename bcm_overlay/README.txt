BCM host-interface overlay for Rockbox (iPod Video 5.5G)
Built against upstream Rockbox commit in .base-commit.

INSTALL (Git Bash, from the ROOT of your Rockbox folder, e.g. K:\bcm-rockbox):
    bash /k/bcm_overlay/apply_overlay.sh        (adjust the path to where you unzipped this)
    git checkout -b bcm-host
    git add -A
    git commit -m "ipodvideo: VideoCore host interface"

Then build (GitHub Actions workflow is included in .github/workflows/, or use WSL) and copy the
result to the iPod. Put your extracted Resources folder on the iPod at /.rockbox/vc/Resources.
On the iPod: Settings -> System -> Debug -> "BCM host test", then send /bcm_host_log.txt.

Files replaced/merged : apps/debug_menu.c, firmware/SOURCES, firmware/target/arm/ipod/video/lcd-video.c
Files added           : firmware/export/bcm_host.h, firmware/target/arm/ipod/video/bcm_host.c,
                        firmware/target/arm/ipod/video/bcm_host_io.h, .github/workflows/build-ipodvideo.yml,
                        tools/bcm/{vc_extract.py,nor_vmcs_check.py,test/bcm_host_test.c}
bcm-host.patch is the same change as a patch (used by the installer to merge into edited files).

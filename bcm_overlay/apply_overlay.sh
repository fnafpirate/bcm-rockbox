#!/usr/bin/env bash
# apply_overlay.sh - drop the finished BCM host-interface files into your Rockbox folder.
#
# Run it from the ROOT of your Rockbox folder (e.g. K:\bcm-rockbox) in Git Bash:
#     bash /k/bcm_overlay/apply_overlay.sh
#
# * Three existing files are replaced: apps/debug_menu.c, firmware/SOURCES,
#   firmware/target/arm/ipod/video/lcd-video.c.  Each is replaced ONLY if it is still identical
#   to upstream Rockbox; if you have edited it, the change is merged in with `git apply` instead,
#   and if that fails the file is left untouched and the finished version is written next to it
#   as <file>.bcm-new for a manual merge.
# * New files are added (an existing different file is saved as <file>.bak first).
set -u
SRC="$(cd "$(dirname "$0")" && pwd)"
[ -f firmware/SOURCES ] && [ -d apps ] && [ -d firmware/target/arm/ipod/video ] || {
    echo "Run this from the root of your Rockbox folder (it must contain apps/ and firmware/)."; exit 1; }

hash() { git hash-object --no-filters "$1" 2>/dev/null || sha1sum "$1" | cut -d' ' -f1; }
conflicts=0

while read -r want f; do
    cur="$(hash "$f")"
    ovl="$(grep " $f\$" "$SRC/.overlay-hashes" | cut -d' ' -f1)"
    if [ "$cur" = "$ovl" ]; then
        echo "already applied : $f"
    elif [ "$cur" = "$want" ]; then
        cp "$SRC/$f" "$f" && echo "replaced        : $f"
    else
        if grep -q $'\r' "$f"; then echo "note: $f has Windows (CRLF) line endings - see README"; fi
        if git apply --include="$f" "$SRC/bcm-host.patch" 2>/dev/null; then
            echo "merged          : $f (you had local edits; the patch merged cleanly)"
        else
            cp "$SRC/$f" "$f.bcm-new"
            echo "CONFLICT        : $f  (left untouched; finished version saved as $f.bcm-new)"
            conflicts=$((conflicts+1))
        fi
    fi
done < "$SRC/.pristine-hashes"

for f in firmware/export/bcm_host.h \
         firmware/target/arm/ipod/video/bcm_host.c \
         firmware/target/arm/ipod/video/bcm_host_io.h \
         .github/workflows/build-ipodvideo.yml \
         tools/bcm/vc_extract.py tools/bcm/nor_vmcs_check.py tools/bcm/test/bcm_host_test.c; do
    mkdir -p "$(dirname "$f")"
    if [ -f "$f" ] && ! cmp -s "$SRC/$f" "$f"; then cp "$f" "$f.bak"; echo "backed up       : $f.bak"; fi
    cp "$SRC/$f" "$f" && echo "added           : $f"
done

echo
if [ "$conflicts" -gt 0 ]; then
    echo "$conflicts file(s) need a manual merge (see CONFLICT lines above). Send them to me if you want help."
    exit 2
fi
echo "Done. Next:  git checkout -b bcm-host && git add -A && git commit -m 'ipodvideo: VideoCore host interface'"

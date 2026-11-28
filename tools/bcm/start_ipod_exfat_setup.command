#!/bin/bash
# macOS: starts iPod exFAT Setup with the rights it needs to write to disks (asks for your Mac password).
# Works with the GitHub-built "iPod exFAT Setup.app" next to this file, or with ipod_exfat_setup.py and Python 3
# from python.org (which includes the Tk window toolkit).
cd "$(dirname "$0")"
if [ -d "iPod exFAT Setup.app" ]; then
    xattr -dr com.apple.quarantine "iPod exFAT Setup.app" 2>/dev/null
    exec sudo "./iPod exFAT Setup.app/Contents/MacOS/iPod exFAT Setup"
fi
PY=$(command -v python3)
[ -x /Library/Frameworks/Python.framework/Versions/Current/bin/python3 ] && PY=/Library/Frameworks/Python.framework/Versions/Current/bin/python3
exec sudo "$PY" ipod_exfat_setup.py

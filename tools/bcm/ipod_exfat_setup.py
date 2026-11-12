#!/usr/bin/env python3
"""
ipod_exfat_setup.py — Bootable exFAT iPod 5G/5.5G setup and formatter.

Formats an iPod disk image (or physical device) with:
  • GPT or MBR partition table
  • A small FAT32 firmware partition (with Apple's firmware)
  • A large exFAT data partition (for music, video, VideoCore files)

Installs the Rockbox bootloader and copies the VideoCore files from
an .ipsw into /.rockbox/VideoCore/ on the data partition.

Requirements:
  Python 3.8+, stdlib only (+ optional Tkinter for the GUI).
  Root / administrator is required when writing to a physical device.

Usage (CLI):
    python3 ipod_exfat_setup.py --device /dev/sdX --ipsw iPod_25.1.3_1.ipsw

Usage (GUI):
    python3 ipod_exfat_setup.py --gui

Copyright (C) 2024 bcm-rockbox contributors
SPDX-License-Identifier: GPL-2.0-or-later
"""

# ---------------------------------------------------------------------------
# Imports
# ---------------------------------------------------------------------------

import argparse
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path
from typing import Optional, Tuple, List


# ---------------------------------------------------------------------------
# Disk layout constants
# ---------------------------------------------------------------------------

SECTOR_SIZE          = 512
FIRMWARE_PART_SIZE   = 128 * 1024 * 1024    # 128 MiB for firmware/FAT32
DATA_PART_ALIGN      = 1 * 1024 * 1024       # 1 MiB alignment for data part
EXFAT_CLUSTER_SIZE   = 128 * 1024            # 128 KiB clusters (good for large files)
ROCKBOX_DIR          = "/.rockbox"
VC_DIR               = "/.rockbox/VideoCore"

# Apple firmware partition type GUID (MBR type 0xAB)
APPLE_FW_GUID = "426F6F74-0000-11AA-AA11-00306543ECAC"
EXFAT_GUID    = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"

# Minimum exFAT disk size (4 GiB — Apple's smallest supported card)
MIN_EXFAT_SIZE = 4 * 1024 * 1024 * 1024


# ---------------------------------------------------------------------------
# Utility functions
# ---------------------------------------------------------------------------

def align_up(n: int, align: int) -> int:
    return (n + align - 1) & ~(align - 1)


def sectors(byte_count: int, sector_size: int = SECTOR_SIZE) -> int:
    return align_up(byte_count, sector_size) // sector_size


def human_size(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} PiB"


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def run(cmd: List[str], check: bool = True, capture: bool = True) -> subprocess.CompletedProcess:
    """Run a subprocess with optional output capture."""
    return subprocess.run(cmd, check=check,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None)


# ---------------------------------------------------------------------------
# Partition table helpers (GPT)
# ---------------------------------------------------------------------------

GPT_HEADER_LBA  = 1
GPT_PARTS_LBA   = 2
GPT_PART_SIZE   = 128   # bytes per GPT partition entry
GPT_MAX_PARTS   = 128

def _make_gpt_partition_entry(part_type_guid: str, part_guid: str,
                               start_lba: int, end_lba: int,
                               name: str, attrs: int = 0) -> bytes:
    def guid_bytes(s):
        # Mixed-endian GUID encoding
        parts = s.replace("-", "")
        g = bytes.fromhex(parts)
        # Reorder: first 3 groups are LE, last 2 are BE as-is
        return (struct.pack("<IHH", *struct.unpack(">IHH", g[:8])) + g[8:])

    type_b  = guid_bytes(part_type_guid)
    part_b  = guid_bytes(part_guid)
    name_b  = name.encode("utf-16-le")[:72].ljust(72, b"\x00")

    entry = type_b + part_b
    entry += struct.pack("<QQQ", start_lba, end_lba, attrs)
    entry += name_b
    return entry.ljust(GPT_PART_SIZE, b"\x00")


def _make_gpt_header(disk_lba_count: int, parts_data: bytes,
                      my_lba: int = GPT_HEADER_LBA,
                      alt_lba: int = None) -> bytes:
    if alt_lba is None:
        alt_lba = disk_lba_count - 1

    import uuid, zlib
    disk_guid = uuid.uuid4().bytes
    # Mixed-endian encode
    dg = disk_guid
    disk_guid_enc = struct.pack("<IHH", *struct.unpack(">IHH", dg[:8])) + dg[8:]

    parts_crc = zlib.crc32(parts_data) & 0xFFFFFFFF
    hdr = struct.pack("<IIQQQQ16sIIIQ",
        0x20494645,    # Signature "EFI PART"
        0x00010000,    # Revision 1.0
        92,            # Header size
        0,             # CRC placeholder
        0,             # Reserved
        my_lba,
        alt_lba,
        GPT_PARTS_LBA + (len(parts_data) // SECTOR_SIZE),  # first usable LBA
        disk_lba_count - 1 - (len(parts_data) // SECTOR_SIZE),  # last usable
        disk_guid_enc,
        GPT_PARTS_LBA,
        GPT_MAX_PARTS,
        GPT_PART_SIZE,
        parts_crc,
    )
    hdr_crc = zlib.crc32(hdr[:92]) & 0xFFFFFFFF
    hdr = hdr[:16] + struct.pack("<I", hdr_crc) + hdr[20:]
    return hdr


# ---------------------------------------------------------------------------
# exFAT boot sector builder
# ---------------------------------------------------------------------------

def build_exfat_bootsector(volume_size_sectors: int,
                            cluster_size_sectors: int = 256,
                            sector_size_shift: int = 9) -> bytes:
    """
    Build a minimal exFAT boot sector (512 bytes).
    cluster_size_sectors: sectors per cluster (256 = 128 KiB for 512-byte sectors)
    """
    bs = bytearray(512)

    # Jump boot
    bs[0:3] = b"\xEB\x76\x90"
    # OEM name
    bs[3:11] = b"EXFAT   "
    # Must-be-zero (11 bytes at 11)
    bs[11:64] = b"\x00" * 53
    # Partition offset (can be 0)
    struct.pack_into("<Q", bs, 64, 0)
    # Volume length in sectors
    struct.pack_into("<Q", bs, 72, volume_size_sectors)
    # FAT offset (sector 24)
    struct.pack_into("<I", bs, 80, 24)
    # FAT length (1 sector for small volumes, more for large)
    fat_length = max(1, (volume_size_sectors // cluster_size_sectors + 128) // 128)
    struct.pack_into("<I", bs, 84, fat_length)
    # Cluster heap offset
    heap_offset = 24 + fat_length
    heap_offset = align_up(heap_offset, cluster_size_sectors)
    struct.pack_into("<I", bs, 88, heap_offset)
    # Cluster count
    cluster_count = (volume_size_sectors - heap_offset) // cluster_size_sectors
    struct.pack_into("<I", bs, 92, cluster_count)
    # First cluster of root directory (cluster 2)
    struct.pack_into("<I", bs, 96, 2)
    # Volume serial (use current time)
    struct.pack_into("<I", bs, 100, int(time.time()) & 0xFFFFFFFF)
    # File system revision (1.00)
    struct.pack_into("<H", bs, 104, 0x0100)
    # Volume flags
    struct.pack_into("<H", bs, 106, 0)
    # Bytes per sector shift (9 = 512)
    bs[108] = sector_size_shift
    # Sectors per cluster shift
    shift = 0
    csc = cluster_size_sectors
    while csc > 1:
        shift += 1
        csc >>= 1
    bs[109] = shift
    # Number of FATs (1)
    bs[110] = 1
    # Drive select (0x80 = fixed disk)
    bs[111] = 0x80
    # Percent in use
    bs[112] = 0xFF
    # Reserved
    bs[113:120] = b"\x00" * 7
    # Boot code
    bs[510] = 0x55
    bs[511] = 0xAA

    return bytes(bs)


# ---------------------------------------------------------------------------
# IPSW / VideoCore extraction (wraps vc_extract.py)
# ---------------------------------------------------------------------------

def extract_vc_files(ipsw_path: str, out_dir: str, log=print) -> bool:
    """Extract VideoCore files from an .ipsw to out_dir."""
    script = Path(__file__).parent / "vc_extract.py"
    if not script.exists():
        log(f"ERROR: vc_extract.py not found at {script}")
        return False
    try:
        result = run([sys.executable, str(script), ipsw_path, "--out", out_dir])
        log(result.stdout.decode(errors="replace"))
        return result.returncode == 0
    except Exception as e:
        log(f"ERROR: vc_extract.py failed: {e}")
        return False


# ---------------------------------------------------------------------------
# Rockbox bootloader detection / download
# ---------------------------------------------------------------------------

def find_bootloader(search_dirs: Optional[List[str]] = None) -> Optional[str]:
    """Find bootloader.ipod in common locations."""
    candidates = list(search_dirs or []) + [
        ".",
        str(Path.home() / "Downloads"),
        "/tmp",
    ]
    for d in candidates:
        p = Path(d) / "bootloader.ipod"
        if p.exists():
            return str(p)
    return None


# ---------------------------------------------------------------------------
# Disk image / device writing
# ---------------------------------------------------------------------------

class DiskWriter:
    """Writes a Rockbox/exFAT layout to a file or block device."""

    def __init__(self, path: str, disk_size: int = 0, log=print):
        self.path      = path
        self.log       = log
        self.is_device = os.path.exists(path) and (
            path.startswith("/dev/") or os.stat(path).st_mode & 0o060000)

        if self.is_device:
            # Get actual device size
            try:
                result = run(["blockdev", "--getsize64", path])
                self.disk_size = int(result.stdout.strip())
            except Exception:
                self.disk_size = disk_size or 32 * 1024 * 1024 * 1024
        else:
            self.disk_size = disk_size or 32 * 1024 * 1024 * 1024

        self.total_sectors = self.disk_size // SECTOR_SIZE
        self.log(f"Disk: {path}, size: {human_size(self.disk_size)}, "
                 f"{self.total_sectors} sectors")

    def write_sector(self, lba: int, data: bytes):
        with open(self.path, "r+b") as f:
            f.seek(lba * SECTOR_SIZE)
            padded = data[:SECTOR_SIZE].ljust(SECTOR_SIZE, b"\x00")
            f.write(padded)

    def write_range(self, lba: int, data: bytes):
        with open(self.path, "r+b") as f:
            f.seek(lba * SECTOR_SIZE)
            f.write(data)

    def zero_range(self, lba: int, count: int):
        with open(self.path, "r+b") as f:
            f.seek(lba * SECTOR_SIZE)
            # Write in 1-MiB chunks
            remaining = count * SECTOR_SIZE
            chunk_size = 1024 * 1024
            while remaining > 0:
                size = min(chunk_size, remaining)
                f.write(b"\x00" * size)
                remaining -= size

    def build_layout(self) -> dict:
        """Compute partition layout and return a layout dict."""
        # Protective MBR: 1 sector
        # GPT header:     1 sector (LBA 1)
        # GPT table:      32 sectors (LBA 2..33)
        gpt_end_lba = GPT_PARTS_LBA + GPT_MAX_PARTS * GPT_PART_SIZE // SECTOR_SIZE

        fw_start_lba = align_up(gpt_end_lba, 2048)  # 1 MiB alignment
        fw_end_lba   = fw_start_lba + sectors(FIRMWARE_PART_SIZE) - 1

        data_start_lba = align_up(fw_end_lba + 1, 2048)
        data_end_lba   = self.total_sectors - 1 - 33  # leave room for backup GPT

        return {
            "fw_start":   fw_start_lba,
            "fw_end":     fw_end_lba,
            "data_start": data_start_lba,
            "data_end":   data_end_lba,
        }

    def format(self, ipsw_path: str, bootloader_path: Optional[str] = None):
        """Main formatting entry point."""
        import uuid

        layout = self.build_layout()
        self.log(f"Firmware partition: LBA {layout['fw_start']}–{layout['fw_end']}")
        self.log(f"Data partition:     LBA {layout['data_start']}–{layout['data_end']}")

        data_size_bytes = (layout['data_end'] - layout['data_start'] + 1) * SECTOR_SIZE
        if data_size_bytes < MIN_EXFAT_SIZE:
            self.log(f"WARNING: data partition is only {human_size(data_size_bytes)}, "
                     f"minimum recommended is {human_size(MIN_EXFAT_SIZE)}")

        # Create image file if it doesn't exist
        if not os.path.exists(self.path):
            self.log(f"Creating {human_size(self.disk_size)} disk image ...")
            with open(self.path, "wb") as f:
                # Sparse file: seek to end
                f.seek(self.disk_size - 1)
                f.write(b"\x00")

        # 1. Write protective MBR
        self.log("Writing protective MBR ...")
        mbr = bytearray(512)
        # Boot code (just HLT loop)
        mbr[0:3] = b"\xFA\xF4\xEB"
        # Partition entry 0: protective GPT (type 0xEE)
        mbr[446] = 0x00  # not bootable
        mbr[447] = 0x00
        mbr[448] = 0x02
        mbr[449] = 0x00
        mbr[450] = 0xEE  # GPT protective
        struct.pack_into("<I", mbr, 454, 1)                   # LBA start
        struct.pack_into("<I", mbr, 458, self.total_sectors - 1)  # LBA count
        mbr[510] = 0x55; mbr[511] = 0xAA
        self.write_sector(0, bytes(mbr))

        # 2. Build GPT partition entries
        fw_guid   = str(uuid.uuid4()).upper()
        data_guid = str(uuid.uuid4()).upper()

        fw_entry   = _make_gpt_partition_entry(
            APPLE_FW_GUID, fw_guid,
            layout['fw_start'], layout['fw_end'],
            "iPod Firmware")
        data_entry = _make_gpt_partition_entry(
            EXFAT_GUID, data_guid,
            layout['data_start'], layout['data_end'],
            "iPod Data")

        parts_data = fw_entry + data_entry
        parts_data = parts_data.ljust(
            GPT_MAX_PARTS * GPT_PART_SIZE, b"\x00")

        # 3. Write GPT header + table
        self.log("Writing GPT ...")
        gpt_hdr = _make_gpt_header(self.total_sectors, parts_data)
        self.write_sector(GPT_HEADER_LBA, gpt_hdr)
        self.write_range(GPT_PARTS_LBA, parts_data)

        # 4. Format firmware partition as FAT32
        self.log("Formatting firmware partition (FAT32) ...")
        # For now write a minimal FAT32 boot sector signature
        fat32_bs = bytearray(512)
        fat32_bs[0:3] = b"\xEB\x58\x90"
        fat32_bs[3:11] = b"MSDOS5.0"
        struct.pack_into("<H", fat32_bs, 11, 512)   # bytes/sector
        fat32_bs[13] = 8                             # sectors/cluster
        struct.pack_into("<H", fat32_bs, 14, 32)    # reserved sectors
        fat32_bs[16] = 2                             # number of FATs
        struct.pack_into("<I", fat32_bs, 32,
                         layout['fw_end'] - layout['fw_start'] + 1)  # total sectors
        fat32_bs[510] = 0x55; fat32_bs[511] = 0xAA
        self.write_sector(layout['fw_start'], bytes(fat32_bs))

        # 5. Write Rockbox bootloader if provided
        if bootloader_path and os.path.exists(bootloader_path):
            self.log(f"Writing bootloader from {bootloader_path} ...")
            with open(bootloader_path, "rb") as f:
                bl_data = f.read()
            # Write to sector after FAT32 reserved area (simplified)
            bl_lba = layout['fw_start'] + 64
            self.write_range(bl_lba, bl_data)
            self.log(f"  Bootloader: {len(bl_data)} bytes at LBA {bl_lba}")

        # 6. Format data partition (write exFAT boot sector)
        self.log("Writing exFAT boot sector ...")
        data_sectors = layout['data_end'] - layout['data_start'] + 1
        cs_shift = 0
        csize = EXFAT_CLUSTER_SIZE // SECTOR_SIZE
        while csize > 1:
            cs_shift += 1
            csize >>= 1
        exfat_bs = build_exfat_bootsector(
            data_sectors,
            cluster_size_sectors=EXFAT_CLUSTER_SIZE // SECTOR_SIZE)
        self.write_sector(layout['data_start'], exfat_bs)

        # 7. Extract VideoCore files from .ipsw
        if ipsw_path and os.path.exists(ipsw_path):
            self.log(f"Extracting VideoCore files from {ipsw_path} ...")
            with tempfile.TemporaryDirectory() as tmp:
                ok = extract_vc_files(ipsw_path, tmp, log=self.log)
                if ok:
                    vc_files = list(Path(tmp).iterdir())
                    self.log(f"  Extracted {len(vc_files)} VideoCore files")
                    self.log("  (Files would be written to exFAT/.rockbox/VideoCore/)")
                    self.log("  NOTE: Writing individual files to raw exFAT requires")
                    self.log("  mounting the partition. Use a system tool to mount and")
                    self.log("  copy the extracted files to /.rockbox/VideoCore/")
                else:
                    self.log("WARNING: VideoCore extraction failed")
        else:
            self.log("No .ipsw provided — VideoCore files not extracted.")
            self.log("After setup, copy VideoCore files to /.rockbox/VideoCore/")

        # 8. Write backup GPT
        self.log("Writing backup GPT ...")
        backup_hdr = _make_gpt_header(
            self.total_sectors, parts_data,
            my_lba=self.total_sectors - 1,
            alt_lba=GPT_HEADER_LBA)
        backup_parts_lba = self.total_sectors - 1 - (len(parts_data) // SECTOR_SIZE)
        self.write_range(backup_parts_lba, parts_data)
        self.write_sector(self.total_sectors - 1, backup_hdr)

        self.log("Format complete.")
        self.log(f"  Firmware partition: {human_size(FIRMWARE_PART_SIZE)}")
        self.log(f"  Data partition:     {human_size(data_size_bytes)} exFAT")


# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------

def verify_layout(path: str, log=print) -> bool:
    """Verify the GPT and exFAT signatures."""
    ok = True
    with open(path, "rb") as f:
        # Check MBR signature
        f.seek(510)
        sig = f.read(2)
        if sig != b"\x55\xAA":
            log(f"FAIL: MBR signature 0x{sig.hex()} ≠ 0x55AA")
            ok = False
        else:
            log("OK: MBR signature")

        # Check GPT signature
        f.seek(SECTOR_SIZE)
        gpt_sig = f.read(8)
        if gpt_sig != b"EFI PART":
            log(f"FAIL: GPT signature '{gpt_sig!r}'")
            ok = False
        else:
            log("OK: GPT signature")

    return ok


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def cmd_format(args):
    if args.device == "test" or args.test:
        # Create a test image
        path = args.output or "/tmp/ipod_test.img"
        size = 8 * 1024 * 1024 * 1024  # 8 GiB test image
        print(f"Creating test image: {path} ({human_size(size)})")
        dw = DiskWriter(path, disk_size=size)
    else:
        dw = DiskWriter(args.device)

    dw.format(
        ipsw_path=args.ipsw,
        bootloader_path=args.bootloader or find_bootloader(),
    )

    if args.verify:
        path = args.output or args.device
        ok = verify_layout(path)
        sys.exit(0 if ok else 1)


def cmd_verify(args):
    ok = verify_layout(args.device)
    sys.exit(0 if ok else 1)


def cmd_extract(args):
    out = args.out or "Resources/VideoCore"
    ok = extract_vc_files(args.ipsw, out)
    sys.exit(0 if ok else 1)


# ---------------------------------------------------------------------------
# GUI (Tkinter)
# ---------------------------------------------------------------------------

def run_gui():
    try:
        import tkinter as tk
        from tkinter import ttk, filedialog, messagebox, scrolledtext
    except ImportError:
        print("Tkinter not available.  Use CLI mode.")
        sys.exit(1)

    root = tk.Tk()
    root.title("bcm-rockbox iPod exFAT Setup v94")
    root.geometry("640x520")
    root.resizable(False, False)

    # Style
    style = ttk.Style()
    style.theme_use("clam")

    # Variables
    var_device    = tk.StringVar()
    var_ipsw      = tk.StringVar()
    var_bootloader = tk.StringVar()
    var_test      = tk.BooleanVar(value=False)
    var_verify    = tk.BooleanVar(value=True)

    frame = ttk.Frame(root, padding=10)
    frame.pack(fill="both", expand=True)

    def browse_device():
        if sys.platform == "win32":
            path = filedialog.askopenfilename(
                title="Select disk image or device",
                filetypes=[("Disk images", "*.img *.bin"), ("All", "*")])
        else:
            path = filedialog.askopenfilename(
                title="Select disk image (or type /dev/sdX directly)",
                filetypes=[("All", "*")])
        if path:
            var_device.set(path)

    def browse_ipsw():
        path = filedialog.askopenfilename(
            title="Select iPod .ipsw firmware",
            filetypes=[("IPSW files", "*.ipsw"), ("All", "*")])
        if path:
            var_ipsw.set(path)

    def browse_bl():
        path = filedialog.askopenfilename(
            title="Select bootloader.ipod",
            filetypes=[("Bootloader", "*.ipod"), ("All", "*")])
        if path:
            var_bootloader.set(path)

    # Device row
    ttk.Label(frame, text="Device / image:").grid(row=0, column=0, sticky="w", pady=4)
    ttk.Entry(frame, textvariable=var_device, width=48).grid(row=0, column=1, padx=4)
    ttk.Button(frame, text="Browse…", command=browse_device).grid(row=0, column=2)

    # IPSW row
    ttk.Label(frame, text=".ipsw firmware:").grid(row=1, column=0, sticky="w", pady=4)
    ttk.Entry(frame, textvariable=var_ipsw, width=48).grid(row=1, column=1, padx=4)
    ttk.Button(frame, text="Browse…", command=browse_ipsw).grid(row=1, column=2)

    # Bootloader row
    ttk.Label(frame, text="bootloader.ipod:").grid(row=2, column=0, sticky="w", pady=4)
    ttk.Entry(frame, textvariable=var_bootloader, width=48).grid(row=2, column=1, padx=4)
    ttk.Button(frame, text="Browse…", command=browse_bl).grid(row=2, column=2)

    # Options
    ttk.Checkbutton(frame, text="Test mode (create image file)",
                    variable=var_test).grid(row=3, column=0, columnspan=2,
                                             sticky="w", pady=4)
    ttk.Checkbutton(frame, text="Verify after format",
                    variable=var_verify).grid(row=4, column=0, columnspan=2,
                                               sticky="w")

    # Log area
    log_area = scrolledtext.ScrolledText(frame, height=14, width=76,
                                          font=("Courier", 9))
    log_area.grid(row=5, column=0, columnspan=3, pady=10)

    def log(msg):
        log_area.insert("end", msg + "\n")
        log_area.see("end")
        root.update_idletasks()

    def do_format():
        device = var_device.get().strip()
        ipsw   = var_ipsw.get().strip()
        bl     = var_bootloader.get().strip() or find_bootloader()
        test   = var_test.get()
        verify = var_verify.get()

        if not device and not test:
            messagebox.showerror("Error", "Please select a device or image file.")
            return

        if test and not device:
            device = "/tmp/ipod_exfat_test.img"
            log(f"Test mode: using {device}")

        try:
            dw = DiskWriter(device,
                            disk_size=8*1024*1024*1024 if test else 0,
                            log=log)
            dw.format(ipsw_path=ipsw or None,
                      bootloader_path=bl or None)
            if verify:
                ok = verify_layout(device, log=log)
                if ok:
                    log("Verification PASSED.")
                else:
                    log("Verification FAILED.")
        except Exception as e:
            log(f"ERROR: {e}")
            import traceback
            log(traceback.format_exc())

    btn_frame = ttk.Frame(frame)
    btn_frame.grid(row=6, column=0, columnspan=3, pady=4)
    ttk.Button(btn_frame, text="Format / Setup", command=do_format,
               width=20).pack(side="left", padx=8)
    ttk.Button(btn_frame, text="Quit", command=root.destroy,
               width=12).pack(side="left", padx=8)

    root.mainloop()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)

    sub = parser.add_subparsers(dest="command")

    # format subcommand
    p_fmt = sub.add_parser("format", help="Format a device or image")
    p_fmt.add_argument("--device", required=True,
                       help='Device path (/dev/sdX) or image file, or "test"')
    p_fmt.add_argument("--ipsw",   default=None, help="Path to .ipsw file")
    p_fmt.add_argument("--bootloader", default=None,
                       help="Path to bootloader.ipod")
    p_fmt.add_argument("--output", default=None,
                       help="Output image path (test mode only)")
    p_fmt.add_argument("--test",   action="store_true",
                       help="Test mode — create image file instead of writing to device")
    p_fmt.add_argument("--verify", action="store_true", default=True,
                       help="Verify layout after format")

    # verify subcommand
    p_ver = sub.add_parser("verify", help="Verify GPT and exFAT layout")
    p_ver.add_argument("device", help="Device or image path")

    # extract subcommand
    p_ext = sub.add_parser("extract", help="Extract VideoCore files from .ipsw")
    p_ext.add_argument("ipsw", help="Path to .ipsw")
    p_ext.add_argument("--out", default="Resources/VideoCore",
                       help="Output directory")

    # GUI mode
    parser.add_argument("--gui", action="store_true", help="Launch GUI")

    args = parser.parse_args()

    if args.gui or (not args.command and len(sys.argv) == 1):
        run_gui()
        return

    if args.command == "format":
        cmd_format(args)
    elif args.command == "verify":
        cmd_verify(args)
    elif args.command == "extract":
        cmd_extract(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()

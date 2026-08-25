#!/usr/bin/env python3
"""
WynlandOS - hand-built MBR sector writer.

Writes a single 512-byte MBR sector describing the two-partition layout
the OS expects (mirrors real-Linux /boot/efi + / conventions):

    partition 1: FAT32 ESP   (type 0xEF) -- what UEFI firmware needs to
                                 load the kernel off its own FAT driver
    partition 2: ext2 root   (type 0x83) -- what drivers/fs/ext2.c mounts

Everything is 1MiB-aligned so dd bs=1M assembly stays trivial and the
WSL<->Windows I/O bridge doesn't crawl (per-phase-20 tooling lesson):
    ESP start  : LBA 2048            (= 1 MiB)
    root start : LBA (1+ESP_MB)*2048 (= right after the ESP, 1MiB-aligned)
    root count : every sector left up to the end of the disk

Usage: python3 make_mbr.py <output-sector-file> <total_image_mb> <esp_mb>

For the defaults (2048 MB disk, 64 MB ESP) this reproduces exactly the
layout the Phase 20a checkpoint verified against live QEMU boots:
    MBR: partition 0 type=0xEF lba_start=2048   sectors=131072
    MBR: partition 1 type=0x83 lba_start=133120 sectors=4061184
"""

import struct
import sys


def main() -> None:
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)

    out_path = sys.argv[1]
    total_mb = int(sys.argv[2])
    esp_mb = int(sys.argv[3])

    if esp_mb < 64:
        # Real OVMF firmware refuses FAT32 volumes below ~65525 clusters;
        # a 32MB ESP passes mtools but not the firmware's own stricter
        # FAT driver (Phase 20a finding). Keep a hard floor.
        print(f"ERROR: ESP partitions must be >= 64MB (got {esp_mb}MB)", file=sys.stderr)
        sys.exit(1)

    SECTORS_PER_MB = 2048  # 512-byte sectors
    total_sectors = total_mb * SECTORS_PER_MB
    esp_start = 2048                      # 1 MiB
    esp_count = esp_mb * SECTORS_PER_MB
    root_start = esp_start + esp_count    # still 1MiB-aligned by construction
    root_count = total_sectors - root_start

    sector = bytearray(512)

    def entry(off: int, ptype: int, lba: int, count: int) -> None:
        e = sector[off:off + 16]
        # status + dummy CHS start (LBA-only table) + type + dummy CHS end
        e[0] = 0x00
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF
        e[4] = ptype
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF
        struct.pack_into("<II", e, 8, lba, count)
        sector[off:off + 16] = e

    # Partition entries live at 0x1BE; no boot code needed -- this disk
    # boots through the UEFI firmware loading the ESP's BOOTX64.EFI.
    entry(0x1BE, 0xEF, esp_start, esp_count)
    entry(0x1CE, 0x83, root_start, root_count)
    sector[0x1FE:0x200] = b"\x55\xAA"

    with open(out_path, "wb") as f:
        f.write(sector)

    print(f"  MBR        ESP: lba={esp_start} sectors={esp_count} | "
          f"root: lba={root_start} sectors={root_count}")


if __name__ == "__main__":
    main()

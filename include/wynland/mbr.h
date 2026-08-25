/*
 * WynlandOS - MBR partition table parsing
 *
 * The disk image is a real MBR-partitioned disk: a small FAT32 ESP
 * (type 0xEF -- what the UEFI firmware's own FAT driver needs to load
 * kernel.elf) plus the ext2 root filesystem (type 0x83). This module
 * parses that table once at boot and exposes where the root partition
 * starts, so every ext2 sector access can be offset by its real LBA
 * start instead of assuming a from-LBA-0 filesystem.
 */
#pragma once

#include <wynland/types.h>

/* Parses LBA 0. Safe to call once during boot, after ahci_init(). */
void mbr_init(void);

/* LBA of the first sector of the root (type 0x83) partition.
   0 means "no root partition was found". */
uint32_t mbr_root_partition_lba(void);

/* Size of the root partition in 512-byte sectors (0 if none). */
uint32_t mbr_root_partition_sectors(void);

/*
 * WynlandOS - which disk is ours.
 *
 * make_mbr.py writes "WYNLANDOS-DISK" into the MBR's boot-code area (the
 * first bytes of LBA 0; a UEFI boot never runs them). The disk drivers
 * (AHCI, NVMe) mount and write only a disk that carries it: on a real
 * machine, booted from a USB stick, the internal Windows disk -- GPT, its
 * protective MBR also ends in 55AA -- is never touched.
 */
#pragma once

#include <wynland/types.h>

static inline bool wyn_disk_is_ours(const uint8_t *lba0)
{
    static const char label[] = "WYNLANDOS-DISK";
    for (int i = 0; i < 14; i++)
        if (lba0[i] != (uint8_t)label[i]) return false;
    return lba0[510] == 0x55 && lba0[511] == 0xAA;
}

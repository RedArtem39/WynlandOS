/*
 * WynlandOS - MBR partition table parsing (implementation)
 *
 * Reads LBA 0, validates the boot signature, and walks the four
 * partition entries at offset 0x1BE. Logs every non-empty entry in a
 * fixed serial format (the same one the Phase 20a checkpoint verified
 * against the values hand-written into the image), records the ESP's
 * presence, and captures the root (type 0x83) partition's LBA start
 * and size for the ext2 driver.
 */

#include <wynland/mbr.h>
#include <wynland/ahci.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern void uint_to_hex(uint64_t val, char *buf);

uint32_t g_root_partition_lba = 0;
uint32_t g_root_partition_sectors = 0;

void mbr_init(void) {
    g_root_partition_lba = 0;
    g_root_partition_sectors = 0;

    uint8_t sector[512];
    if (!ahci_read(0, 1, sector)) {
        serial_write_string("MBR: failed to read LBA 0!\r\n");
        return;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        serial_write_string("MBR: no valid boot signature at LBA 0!\r\n");
        return;
    }

    bool have_esp = false;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sector + 0x1BE + i * 16;
        uint8_t  type = e[4];
        uint32_t lba = (uint32_t)e[8]  | ((uint32_t)e[9]  << 8) |
                       ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t cnt = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                       ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (type == 0x00) continue;

        serial_write_string("MBR: partition ");
        char buf[16];
        uint_to_str((uint64_t)i, buf);
        serial_write_string(buf);
        serial_write_string(" type=0x");
        uint_to_hex(type, buf);
        serial_write_string(buf);
        serial_write_string(" lba_start=");
        uint_to_str(lba, buf);
        serial_write_string(buf);
        serial_write_string(" sectors=");
        uint_to_str(cnt, buf);
        serial_write_string(buf);
        serial_write_string("\r\n");

        if (type == 0xEF) have_esp = true;
        if (type == 0x83 && g_root_partition_lba == 0) {
            g_root_partition_lba = lba;
            g_root_partition_sectors = cnt;
        }
    }

    if (!have_esp)
        serial_write_string("MBR: warning -- no ESP (type 0xEF) partition found\r\n");
    if (g_root_partition_lba == 0)
        serial_write_string("MBR: error -- no root (type 0x83) partition found!\r\n");
}

uint32_t mbr_root_partition_lba(void) {
    return g_root_partition_lba;
}

uint32_t mbr_root_partition_sectors(void) {
    return g_root_partition_sectors;
}

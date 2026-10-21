/*
 * WynlandOS - NVMe Driver Header
 */
#pragma once

#include <wynland/types.h>

bool nvme_init(void);
bool nvme_ok(void);
bool nvme_read(uint32_t lba, uint32_t count, void *buf);
bool nvme_write(uint32_t lba, uint32_t count, const void *buf);

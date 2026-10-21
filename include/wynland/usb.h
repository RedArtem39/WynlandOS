/*
 * WynlandOS - USB (xHCI) Driver Header
 */
#pragma once

#include <wynland/types.h>

void usb_init(void);
bool usb_ready(void);

bool usb_storage_ready(void);
uint64_t usb_storage_sectors(void);
uint32_t usb_storage_sector_size(void);
int usb_storage_read_bytes(uint64_t off, void *buf, uint32_t len);
int usb_storage_write_bytes(uint64_t off, const void *buf, uint32_t len);

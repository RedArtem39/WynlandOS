/*
 * WynlandOS - ACPI core: tables, FADT power/reset, MADT/MCFG access
 */
#pragma once

#include <wynland/types.h>

/* Parse RSDP -> XSDT/RSDT and copy the tables we use into the kernel
   heap. Returns false only when there is no usable root table. */
bool acpi_init(uint64_t rsdp_phys);

bool acpi_available(void);

/* Find a table by its 4-character signature ("FACP", "APIC", "MCFG", ...).
   The returned bytes belong to the kernel and stay valid for good. */
const void *acpi_find_table(const char *sig);

uint32_t acpi_table_count(void);

/* PCI configuration space through MCFG/ECAM. Return false when the bus is
   not covered (the caller should fall back to the 0xCF8/0xCFC ports). */
bool acpi_pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off,
                       uint32_t *out);
bool acpi_pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off,
                        uint32_t val);

/* Enter the S5 (soft off) state. Returns false if ACPI is absent or the
   write did not take effect (the caller then falls back to port tricks). */
bool acpi_poweroff(void);

/* Reset the machine via FADT RESET_REG when present, else 0xCF9. Falls
   back to the caller's keyboard-controller pulse if it returns. */
void acpi_reset(void);

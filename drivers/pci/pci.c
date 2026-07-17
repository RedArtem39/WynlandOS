/*
 * WynlandOS - PCI Bus Driver Implementation
 */

#include <wynland/pci.h>

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) |
                                  (func << 8) | (offset & 0xfc) | ((uint32_t)0x80000000));
    outl(0xCF8, address);
    return inl(0xCFC);
}

void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) |
                                  (func << 8) | (offset & 0xfc) | ((uint32_t)0x80000000));
    outl(0xCF8, address);
    outl(0xCFC, value);
}

bool pci_find_device(uint8_t class_code, uint8_t subclass, PciDevice *out_dev) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t reg0 = pci_read_config(bus, slot, func, 0x00);
                uint16_t vendor_id = reg0 & 0xFFFF;
                if (vendor_id == 0xFFFF) {
                    if (func == 0) break; // If function 0 is not present, skip other functions
                    continue;
                }
                
                uint32_t reg8 = pci_read_config(bus, slot, func, 0x08);
                uint8_t dev_class = (reg8 >> 24) & 0xFF;
                uint8_t dev_subclass = (reg8 >> 16) & 0xFF;
                uint8_t dev_prog_if = (reg8 >> 8) & 0xFF;
                
                if (dev_class == class_code && dev_subclass == subclass) {
                    out_dev->bus = bus;
                    out_dev->slot = slot;
                    out_dev->func = func;
                    out_dev->vendor_id = vendor_id;
                    out_dev->device_id = (reg0 >> 16) & 0xFFFF;
                    out_dev->class_code = dev_class;
                    out_dev->subclass = dev_subclass;
                    out_dev->prog_if = dev_prog_if;
                    out_dev->bar5 = pci_read_config(bus, slot, func, 0x24);
                    return true;
                }
            }
        }
    }
    return false;
}

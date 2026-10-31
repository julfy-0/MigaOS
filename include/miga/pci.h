/* Минимальный доступ к PCI через порты 0xCF8/0xCFC (Configuration Mechanism #1). */
#pragma once
#include <stdint.h>
#include <miga/io.h>

#define PCI_CMD 0x04
#define PCI_BAR0 0x10

static inline uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(0xCF8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (off & 0xFC));
    return inl(0xCFC);
}

static inline void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    outl(0xCF8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (off & 0xFC));
    outl(0xCFC, v);
}

/* Ищет первое устройство с кодом класса (class<<16 | subclass<<8 | prog_if). 0 = найдено. */
static inline int pci_find_class(uint32_t code, uint8_t *bus, uint8_t *dev, uint8_t *fn)
{
    for (unsigned b = 0; b < 256; b++)
        for (unsigned d = 0; d < 32; d++)
            for (unsigned f = 0; f < 8; f++) {
                if ((pci_read32(b, d, f, 0) & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                if ((pci_read32(b, d, f, 8) >> 8) == code) { *bus = b; *dev = d; *fn = f; return 0; }
                if (f == 0 && !(pci_read32(b, d, f, 0x0C) & 0x00800000)) break; /* не multifunction */
            }
    return -1;
}

/* Перечисление PCI-шины: лог при загрузке и команда lspci [-v]. */
#include <kernel/kernel.h>
#include <miga/pci.h>

static const struct { uint16_t id; const char *name; } vendors[] = {
    {0x8086, "Intel"}, {0x1022, "AMD"}, {0x1002, "AMD/ATI"}, {0x10DE, "NVIDIA"}, {0x10EC, "Realtek"},
    {0x14E4, "Broadcom"}, {0x168C, "Qualcomm Atheros"}, {0x17CB, "Qualcomm"}, {0x14C3, "MediaTek"},
    {0x1969, "Qualcomm Atheros"}, {0x11AB, "Marvell"}, {0x1B21, "ASMedia"}, {0x1912, "Renesas"},
    {0x144D, "Samsung"}, {0x15B7, "SanDisk"}, {0x1C5C, "SK hynix"}, {0x1987, "Phison"}, {0x1CC1, "ADATA"},
    {0x1E0F, "KIOXIA"}, {0x1B4B, "Marvell"}, {0x1106, "VIA"}, {0x1B36, "QEMU/Red Hat"}, {0x1AF4, "virtio"},
    {0x1234, "QEMU"}, {0x15AD, "VMware"}, {0x80EE, "VirtualBox"}, {0x1414, "Microsoft"}, {0x1D6A, "Aquantia"},
    {0x15B3, "Mellanox"}, {0x1AE0, "Google"},
};

static const struct { uint16_t v, d; const char *n; } devs[] = {
    {0x1234, 0x1111, "VGA (Bochs/QEMU std)"}, {0x8086, 0x1237, "440FX host bridge"}, {0x8086, 0x7000, "PIIX3 ISA bridge"},
    {0x8086, 0x29C0, "Q35 host bridge"}, {0x8086, 0x2918, "ICH9 LPC bridge"}, {0x8086, 0x2922, "ICH9 SATA (AHCI)"},
    {0x8086, 0x2930, "ICH9 SMBus"}, {0x8086, 0x293E, "ICH9 HD Audio"}, {0x8086, 0x100E, "82540EM Gigabit"},
    {0x8086, 0x10D3, "82574L Gigabit"}, {0x1B36, 0x000D, "QEMU XHCI"}, {0x1B36, 0x0010, "QEMU NVMe"},
    {0x1B36, 0x0001, "QEMU PCI-PCI bridge"}, {0x1AF4, 0x1000, "virtio-net (legacy)"}, {0x1AF4, 0x1041, "virtio-net"},
    {0x1AF4, 0x1001, "virtio-blk (legacy)"}, {0x1AF4, 0x1042, "virtio-blk"}, {0x1AF4, 0x1050, "virtio-gpu"},
    {0x8086, 0x2723, "Wi-Fi 6 AX200"}, {0x8086, 0x2725, "Wi-Fi 6E AX210"}, {0x10EC, 0x8168, "RTL8111/8168 Gigabit"},
};

static const char *class_name(unsigned c, unsigned sc)
{
    switch (c) {
    case 0x00: return "legacy";
    case 0x01: return sc == 0x01 ? "IDE" : sc == 0x06 ? "SATA" : sc == 0x07 ? "SAS" : sc == 0x08 ? "NVMe" : "storage";
    case 0x02: return sc == 0x00 ? "Ethernet" : sc == 0x80 ? "network (Wi-Fi?)" : "network";
    case 0x03: return sc == 0x00 ? "VGA" : sc == 0x02 ? "3D controller" : "display";
    case 0x04: return sc == 0x01 ? "audio" : sc == 0x03 ? "HD Audio" : "multimedia";
    case 0x05: return "memory";
    case 0x06: return sc == 0x00 ? "host bridge" : sc == 0x01 ? "ISA bridge" : sc == 0x04 ? "PCI bridge" : "bridge";
    case 0x07: return "communication";
    case 0x08: return "system peripheral";
    case 0x09: return "input";
    case 0x0C: return sc == 0x03 ? "USB" : sc == 0x05 ? "SMBus" : "serial bus";
    case 0x0D: return "wireless";
    case 0x11: return "signal processing";
    case 0x12: return "accelerator";
    }
    return "other";
}

static const char *cap_name(unsigned id)
{
    switch (id) {
    case 0x01: return "PM"; case 0x05: return "MSI"; case 0x07: return "PCI-X"; case 0x09: return "vendor";
    case 0x0D: return "SSVID"; case 0x10: return "PCIe"; case 0x11: return "MSI-X"; case 0x12: return "SATA";
    case 0x13: return "AF";
    }
    return "?";
}

static const char *gts(unsigned s)
{
    switch (s) { case 1: return "2.5"; case 2: return "5"; case 3: return "8"; case 4: return "16"; case 5: return "32"; case 6: return "64"; }
    return "?";
}

static void verbose(unsigned b, unsigned d, unsigned f, uint32_t id)
{
    uint32_t hdr = (pci_read32(b, d, f, 0x0C) >> 16) & 0x7F, sub = pci_read32(b, d, f, 0x2C), irq = pci_read32(b, d, f, 0x3C);
    kprintf("      ");
    if (hdr == 0) kprintf("subsys %04x:%04x, ", sub & 0xFFFF, sub >> 16);
    if ((irq & 0xFF) != 0xFF && (irq >> 8 & 0xFF)) kprintf("IRQ %u pin %c, ", irq & 0xFF, (int)('A' + (irq >> 8 & 0xFF) - 1));
    (void)id;
    kprintf("caps:");
    if (pci_read32(b, d, f, 0x04) & (1u << 20)) {
        uint8_t p = pci_read32(b, d, f, 0x34) & 0xFC;
        for (unsigned i = 0; p && i < 48; i++) {
            uint32_t h = pci_read32(b, d, f, p);
            kprintf(" %s", cap_name(h & 0xFF));
            if ((h & 0xFF) == 0x10) {
                uint32_t cap = pci_read32(b, d, f, p + 0x0C), st = pci_read32(b, d, f, p + 0x10) >> 16;
                kprintf("(%s GT/s x%u, max %s x%u)", gts(st & 0xF), (st >> 4) & 0x3F, gts(cap & 0xF), (cap >> 4) & 0x3F);
            }
            p = (h >> 8) & 0xFC;
        }
    } else kprintf(" none");
    kprintf("\n");
    if (hdr != 0) return;
    for (unsigned i = 0; i < 6; i++) {
        uint32_t lo = pci_read32(b, d, f, 0x10 + 4 * i);
        if (!lo) continue;
        if (lo & 1) { kprintf("      BAR%u io %#x\n", i, lo & ~3u); continue; }
        int is64 = ((lo >> 1) & 3) == 2 && i < 5;
        uint64_t base = lo & ~0xFull;
        if (is64) base |= (uint64_t)pci_read32(b, d, f, 0x10 + 4 * (i + 1)) << 32;
        kprintf("      BAR%u mem%s%s %#lx\n", i, is64 ? "64" : "32", (lo & 8) ? " prefetch" : "", base);
        if (is64) i++;
    }
}

void pci_log_ex(int v)
{
    unsigned n = 0;
    kprintf("\n:: pci%s\n", v ? " (verbose)" : "");
    for (unsigned b = 0; b < 256; b++)
        for (unsigned d = 0; d < 32; d++)
            for (unsigned f = 0; f < 8; f++) {
                uint32_t id = pci_read32(b, d, f, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                uint32_t cl = pci_read32(b, d, f, 8);
                const char *vn = 0, *dn = 0;
                for (unsigned i = 0; i < sizeof vendors / sizeof *vendors; i++) if (vendors[i].id == (id & 0xFFFF)) { vn = vendors[i].name; break; }
                for (unsigned i = 0; i < sizeof devs / sizeof *devs; i++) if (devs[i].v == (id & 0xFFFF) && devs[i].d == (id >> 16)) { dn = devs[i].n; break; }
                kprintf("  %02x:%02x.%u  %04x:%04x  %s", b, d, f, id & 0xFFFF, id >> 16, class_name(cl >> 24, (cl >> 16) & 0xFF));
                if (vn) kprintf("  %s", vn);
                if (dn) kprintf(" %s", dn);
                kprintf("\n");
                if (v) verbose(b, d, f, id);
                n++;
                if (f == 0 && !(pci_read32(b, d, f, 0x0C) & 0x00800000)) break;
            }
    kprintf("  %u device(s)\n", n);
}

void pci_log(void) { pci_log_ex(0); }

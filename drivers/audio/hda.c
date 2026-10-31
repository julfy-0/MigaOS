/* Intel High Definition Audio (PCI class 04h/03h, у старых — 04h/01h): регистры контроллера и ID кодеков
 * через Immediate Command Interface (без CORB/RIRB). Контроллер не сбрасываем: берём то, что оставила прошивка. */
#define MIGA_TAG "hda"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/mem.h>
#include <miga/io.h>
#include <miga/log.h>
#include <miga/string.h>
#include <miga/audio.h>

static struct audio_info ctrls[AUDIO_MAX];
static unsigned nctrl;

unsigned audio_count(void) { return nctrl; }

int audio_get(unsigned idx, struct audio_info *out)
{
    if (idx >= nctrl) return -1;
    *out = ctrls[idx];
    return 0;
}

static const char *vendor_name(unsigned v)
{
    switch (v) {
    case 0x8086: return "Intel";   case 0x10EC: return "Realtek";   case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";  case 0x1022: return "AMD";       case 0x14F1: return "Conexant";
    case 0x111D: return "IDT";     case 0x1106: return "VIA";       case 0x1013: return "Cirrus Logic";
    case 0x11D4: return "Analog Devices"; case 0x1AF4: return "QEMU/virtio"; case 0x1B36: return "QEMU";
    }
    return "unknown";
}

static int icmd(volatile uint8_t *r, uint32_t verb, uint32_t *resp)
{
    volatile uint32_t *icoi = (volatile uint32_t *)(r + 0x60), *irii = (volatile uint32_t *)(r + 0x64);
    volatile uint16_t *ics = (volatile uint16_t *)(r + 0x68);
    for (unsigned i = 0; i < 1000 && (*ics & 1); i++) io_wait();     /* предыдущая команда ещё идёт */
    *icoi = verb;
    *ics = 3;                                                        /* сбросить IRV (RW1C) и запустить ICB */
    for (unsigned i = 0; i < 2000; i++) {
        uint16_t s = *ics;
        if (!(s & 1) && (s & 2)) { *resp = *irii; return 0; }
        io_wait();
    }
    return -1;
}

static void print_one(const struct audio_info *a, int tagged)
{
    const char *t = tagged ? "[hda] " : "";
    kprintf("%s%02x:%02x.%u %04x:%04x HD Audio %u.%u (%s): %u out / %u in / %u bidir stream(s)%s, %u codec(s)\n", t, a->pci_bus,
            a->pci_dev, a->pci_fn, a->vendor, a->device, a->vmaj, a->vmin, vendor_name(a->vendor), a->oss, a->iss, a->bss,
            a->ok64 ? ", 64-bit DMA" : "", a->ncodecs);
    for (unsigned i = 0; i < a->ncodecs; i++) {
        const struct audio_codec *c = &a->codec[i];
        kprintf("%s        codec %u: %04x:%04x rev %08x  %s", t, c->addr, c->vendor, c->device, c->rev, vendor_name(c->vendor));
        if (c->vendor == 0x10EC && (c->device >> 12) == 0) kprintf(" ALC%03x", c->device & 0xFFF);   /* Realtek: ID = номер модели в hex */
        kprintf("\n");
    }
}

void audio_print_all(void)
{
    if (!nctrl) { kprintf("audio: no HD Audio controllers found\n"); return; }
    for (unsigned i = 0; i < nctrl; i++) print_one(&ctrls[i], 0);
}

static void scan(unsigned cls)
{
    for (unsigned b = 0; b < 256; b++)
        for (unsigned d = 0; d < 32; d++)
            for (unsigned f = 0; f < 8; f++) {
                uint32_t id = pci_read32(b, d, f, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                uint32_t c = pci_read32(b, d, f, 8) >> 8;
                if ((c >> 8) == cls && nctrl < AUDIO_MAX) {
                    uint32_t cmd = pci_read32(b, d, f, PCI_CMD) & 0xFFFF;
                    pci_write32(b, d, f, PCI_CMD, cmd | 2);          /* включить декодирование памяти */
                    uint32_t b0 = pci_read32(b, d, f, PCI_BAR0);
                    uint64_t bar = b0 & ~0xFull;
                    if (((b0 >> 1) & 3) == 2) bar |= (uint64_t)pci_read32(b, d, f, PCI_BAR0 + 4) << 32;
                    volatile uint8_t *r = bar ? mmio_map(bar, 0x4000) : 0;
                    if (r && *(volatile uint16_t *)r != 0xFFFF) {
                        struct audio_info *a = &ctrls[nctrl];
                        memset(a, 0, sizeof *a);
                        uint16_t gcap = *(volatile uint16_t *)r;
                        a->pci_bus = (uint8_t)b; a->pci_dev = (uint8_t)d; a->pci_fn = (uint8_t)f;
                        a->vendor = id & 0xFFFF; a->device = id >> 16;
                        a->oss = (gcap >> 12) & 0xF; a->iss = (gcap >> 8) & 0xF; a->bss = (gcap >> 3) & 0x1F; a->ok64 = gcap & 1;
                        a->vmin = r[0x02]; a->vmaj = r[0x03];
                        if (*(volatile uint32_t *)(r + 0x08) & 1) {  /* контроллер не в сбросе: STATESTS актуален */
                            uint16_t st = *(volatile uint16_t *)(r + 0x0E);
                            for (unsigned k = 0; k < 15 && a->ncodecs < AUDIO_CODECS; k++) {
                                if (!(st >> k & 1)) continue;
                                uint32_t vid, rev;
                                if (icmd((volatile uint8_t *)r, (uint32_t)k << 28 | 0xF00u << 8 | 0x00, &vid)) continue;
                                if (icmd((volatile uint8_t *)r, (uint32_t)k << 28 | 0xF00u << 8 | 0x02, &rev)) rev = 0;
                                a->codec[a->ncodecs++] = (struct audio_codec){(uint8_t)k, (uint16_t)(vid >> 16), (uint16_t)vid, rev};
                            }
                        }
                        print_one(a, 1);
                        nctrl++;
                    }
                }
                if (f == 0 && !(pci_read32(b, d, f, 0x0C) & 0x00800000)) break;
            }
}

static int hda_probe(const struct boot_info *bi)
{
    (void)bi;
    scan(0x0403);                                                    /* HD Audio */
    return nctrl ? 0 : -1;
}

MIGA_DRIVER(hda, "hda", DRV_VER(0, 1), DRV_NORMAL, hda_probe);

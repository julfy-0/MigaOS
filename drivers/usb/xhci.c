/* Драйвер xHCI (USB 3.x/2.0 host controller) + минимальный USB-стек:
 * сброс контроллера, перечисление корневых портов, дескрипторы, поиск HID boot-клавиатур и мышей.
 * Работает по опросу (без прерываний), хабы не поддерживаются. HID boot-клавиатуры и boot-мыши. Всё общение с ядром — через
 * include/miga/ (PCI, DMA-страницы, MMIO). */
#define MIGA_TAG "xhci"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/mem.h>
#include <miga/io.h>
#include <miga/log.h>
#include <miga/string.h>
#include <miga/usb.h>
#include "xhci.h"

/* ------------------------------------------------------------------ константы */
#define RING_TRBS 256
#define MAX_DEVS  8
#define PAGE      4096

enum { TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
       TRB_ENABLE_SLOT = 9, TRB_ADDR_DEV = 11, TRB_CFG_EP = 12, TRB_EVAL_CTX = 13,
       EV_XFER = 32, EV_CMD = 33 };

#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_TC  (1u << 1)
#define TRB_ISP (1u << 2)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR (1u << 16)

#define PORTSC_CCS (1u << 0)
#define PORTSC_PED (1u << 1)
#define PORTSC_PR  (1u << 4)
#define PORTSC_PRC (1u << 21)
#define PORTSC_NEUTRAL ((0xFu << 5) | (1u << 9) | (3u << 14) | (7u << 25)) /* RW-биты, которые надо сохранять */
#define PORTSC_CHANGES 0x00FE0000u                                        /* RW1C-биты изменений */

#define barrier() __asm__ volatile("" ::: "memory")

/* ------------------------------------------------------------------ MMIO */
static inline uint32_t r32(const void *base, uint32_t off) { return *(volatile const uint32_t *)((const uint8_t *)base + off); }
static inline void w32(void *base, uint32_t off, uint32_t v) { *(volatile uint32_t *)((uint8_t *)base + off) = v; }
static inline void w64(void *base, uint32_t off, uint64_t v) { w32(base, off, (uint32_t)v); w32(base, off + 4, (uint32_t)(v >> 32)); }

static void udelay(unsigned us) { while (us--) io_wait(); }

static int wait_reg(const void *base, uint32_t off, uint32_t mask, uint32_t val, unsigned ms)
{
    for (unsigned i = 0; i < ms * 1000; i++) {
        if ((r32(base, off) & mask) == val) return 0;
        io_wait();
    }
    return -1;
}

/* ------------------------------------------------------------------ кольца TRB */
struct trb { uint32_t p_lo, p_hi, status, ctrl; };

struct ring {
    volatile struct trb *trb;
    uint64_t phys;
    unsigned enq;
    uint32_t cycle;
};

static int ring_init(struct ring *r)
{
    r->trb = dma_alloc_page();
    if (!r->trb) return -1;
    r->phys = (uint64_t)r->trb;
    r->enq = 0;
    r->cycle = 1;
    volatile struct trb *l = &r->trb[RING_TRBS - 1]; /* последний TRB — Link на начало */
    l->p_lo = (uint32_t)r->phys;
    l->p_hi = (uint32_t)(r->phys >> 32);
    l->status = 0;
    l->ctrl = TRB_TYPE(TRB_LINK) | TRB_TC;
    return 0;
}

/* Кладёт TRB в кольцо, возвращает его физический адрес (по нему приходит событие). */
static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t ctrl)
{
    volatile struct trb *t = &r->trb[r->enq];
    uint64_t at = r->phys + (uint64_t)r->enq * sizeof(struct trb);
    t->p_lo = (uint32_t)param;
    t->p_hi = (uint32_t)(param >> 32);
    t->status = status;
    barrier();
    t->ctrl = ctrl | r->cycle;               /* cycle-бит отдаёт TRB контроллеру: пишем последним */
    if (++r->enq == RING_TRBS - 1) {
        volatile struct trb *l = &r->trb[r->enq];
        l->ctrl = (l->ctrl & ~1u) | r->cycle; /* отдаём Link-TRB и переворачиваем цикл */
        r->enq = 0;
        r->cycle ^= 1;
    }
    return at;
}

/* ------------------------------------------------------------------ состояние контроллера */
static struct {
    uint8_t *cap, *op, *rt;
    uint32_t *db;
    unsigned ctx_size, max_slots, max_ports, ver;
    uint64_t phys;
    uint64_t *dcbaa;
    struct ring cmd;
    volatile struct trb *ev;
    uint64_t ev_phys;
    unsigned ev_idx;
    uint32_t ev_ccs;
} X;

struct usb_dev {
    uint8_t slot, speed, port;
    uint8_t *in_ctx, *out_ctx, *buf;
    struct ring ep0;
    struct usb_dev_info info;                      /* для команды usb в kshell */
};

struct usb_kbd {                                   /* любое HID boot-устройство: клавиатура или мышь */
    struct usb_dev *dev;
    uint8_t iface, ep, cfg, interval, dci, active, kind;
    uint16_t mps, rlen;
    struct ring ring;
    uint8_t *rep;
    usb_hid_cb cb;
};

static struct usb_dev devs[MAX_DEVS];
static struct usb_kbd kbds[USB_MAX_HID];
static unsigned ndevs, nkbds;

static volatile int cmd_done, ctl_done;
static volatile uint32_t cmd_cc, cmd_slot, ctl_cc;
static volatile uint64_t ctl_expect;

static uint32_t *ctx(uint8_t *base, unsigned i) { return (uint32_t *)(base + (size_t)i * X.ctx_size); }

/* ------------------------------------------------------------------ события */
static void kbd_queue(struct usb_kbd *k)
{
    memset(k->rep, 0, k->rlen);
    ring_push(&k->ring, (uint64_t)k->rep, k->rlen, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    w32(X.db, 4u * k->dev->slot, k->dci);
}

static void on_event(volatile struct trb *t)
{
    uint64_t ptr = ((uint64_t)t->p_hi << 32) | t->p_lo;
    uint32_t st = t->status, ct = t->ctrl;
    unsigned type = (ct >> 10) & 63;

    if (type == EV_CMD) { cmd_cc = st >> 24; cmd_slot = ct >> 24; cmd_done = 1; return; }
    if (type != EV_XFER) return; /* port status change и пр. нам не нужны */

    unsigned slot = ct >> 24, ep = (ct >> 16) & 31, cc = st >> 24;
    if (ep == 1) {               /* EP0: интересует только событие нашего Status-TRB */
        if (ptr == ctl_expect) { ctl_cc = cc; ctl_done = 1; }
        return;
    }
    for (unsigned i = 0; i < nkbds; i++) {
        struct usb_kbd *k = &kbds[i];
        if (!k->active || k->dev->slot != slot || k->dci != ep) continue;
        if (cc == 1 || cc == 13) {
            unsigned len = k->rlen - (st & 0xFFFFFF);
            if (len > k->rlen) len = k->rlen;
            if (k->cb) k->cb(i, k->rep, len);
            kbd_queue(k);
        } else {
            pr_warn("%s %u stopped (cc=%u)\n", k->kind == HID_MOUSE ? "mouse" : "keyboard", i, cc);
            k->active = 0;
        }
    }
}

static void drain(void)
{
    for (;;) {
        volatile struct trb *t = &X.ev[X.ev_idx];
        if ((t->ctrl & 1) != X.ev_ccs) break;
        barrier();
        on_event(t);
        if (++X.ev_idx == RING_TRBS) { X.ev_idx = 0; X.ev_ccs ^= 1; }
        w64(X.rt, 0x38, (X.ev_phys + (uint64_t)X.ev_idx * sizeof(struct trb)) | 8); /* ERDP + EHB */
    }
}

void xhci_poll(void) { if (X.op) drain(); }

static int pump(volatile int *flag, unsigned ms)
{
    for (unsigned i = 0; i < ms * 100; i++) { /* шаг ~10 мкс */
        drain();
        if (*flag) return 0;
        udelay(10);
    }
    return -1;
}

/* ------------------------------------------------------------------ команды и control-передачи */
static int command(uint64_t param, uint32_t ctrl, uint32_t *slot_out)
{
    cmd_done = 0;
    ring_push(&X.cmd, param, 0, ctrl);
    w32(X.db, 0, 0);
    if (pump(&cmd_done, 500)) return -1;
    if (slot_out) *slot_out = cmd_slot;
    return cmd_cc == 1 ? 0 : -(int)cmd_cc;
}

static int ctrl_xfer(struct usb_dev *d, uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, int in)
{
    uint64_t setup = (uint64_t)bm | (uint64_t)req << 8 | (uint64_t)val << 16 | (uint64_t)idx << 32 | (uint64_t)len << 48;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;

    ctl_done = 0;
    ring_push(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (len) ring_push(&d->ep0, (uint64_t)d->buf, len, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR : 0));
    ctl_expect = ring_push(&d->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | ((in && len) ? 0 : TRB_DIR));
    w32(X.db, 4u * d->slot, 1);
    if (pump(&ctl_done, 1000)) return -1;
    return ctl_cc == 1 ? 0 : -(int)ctl_cc;
}

static int get_descriptor(struct usb_dev *d, uint8_t type, uint16_t len)
{
    return ctrl_xfer(d, 0x80, 6, (uint16_t)(type << 8), 0, len, 1);
}

/* ------------------------------------------------------------------ перечисление */
static unsigned ep_interval(unsigned speed, unsigned bint)
{
    unsigned v = 0;
    if (speed >= 3) { v = bint ? bint - 1 : 0; }              /* HS/SS: 2^(bInterval-1) микрокадров */
    else { unsigned us8 = (bint ? bint : 1) * 8; while ((1u << (v + 1)) <= us8) v++; } /* FS/LS: мс -> 125 мкс */
    return v > 15 ? 15 : v;
}

static void parse_config(struct usb_dev *d, const uint8_t *c, unsigned len)
{
    int is_hid = 0;                                            /* 0 / HID_KBD / HID_MOUSE для текущего интерфейса */
    uint8_t iface = 0, cfg = c[5];
    for (unsigned i = 0; i + 2 <= len && c[i]; i += c[i]) {
        if (c[i + 1] == 4 && c[i] >= 9) {                      /* interface: HID / boot / keyboard */
            is_hid = (c[i + 5] == 3 && c[i + 6] == 1 && (c[i + 7] == 1 || c[i + 7] == 2)) ? c[i + 7] : 0;
            iface = c[i + 2];
            if (c[i + 3] == 0) {                               /* alt-setting 0: считаем интерфейс */
                if (d->info.nifaces < USB_INFO_IFACES) {
                    d->info.iface[d->info.nifaces][0] = c[i + 5];
                    d->info.iface[d->info.nifaces][1] = c[i + 6];
                    d->info.iface[d->info.nifaces][2] = c[i + 7];
                }
                if (d->info.nifaces < 255) d->info.nifaces++;
            }
        } else if (c[i + 1] == 5 && c[i] >= 7 && is_hid &&      /* endpoint: interrupt IN */
                   (c[i + 2] & 0x80) && (c[i + 3] & 3) == 3 && nkbds < USB_MAX_HID) {
            struct usb_kbd *k = &kbds[nkbds++];
            k->dev = d; k->iface = iface; k->cfg = cfg; k->ep = c[i + 2] & 0x0F; k->kind = (uint8_t)is_hid;
            k->mps = (uint16_t)((c[i + 4] | c[i + 5] << 8) & 0x7FF);
            k->rlen = k->mps < 8 ? 8 : (k->mps > 64 ? 64 : k->mps);   /* отчёт мыши может быть длиннее 8 байт */
            k->interval = c[i + 6];
            if (is_hid == HID_KBD) d->info.is_kbd = 1; else d->info.is_mouse = 1;
            is_hid = 0;
        }
    }
}

static int enum_port(unsigned port)
{
    uint32_t off = 0x400 + 0x10 * (port - 1);
    uint32_t sc = r32(X.op, off);
    if (!(sc & PORTSC_CCS) || ndevs >= MAX_DEVS) return 0;

    w32(X.op, off, (sc & PORTSC_NEUTRAL) | PORTSC_PR);          /* сброс порта */
    if (wait_reg(X.op, off, PORTSC_PRC, PORTSC_PRC, 500)) return -1;
    sc = r32(X.op, off);
    w32(X.op, off, (sc & PORTSC_NEUTRAL) | PORTSC_CHANGES);     /* сбросить флаги изменений */
    udelay(10000);
    sc = r32(X.op, off);
    if (!(sc & PORTSC_CCS) || !(sc & PORTSC_PED)) return -1;
    unsigned speed = (sc >> 10) & 0xF;                          /* 1=FS 2=LS 3=HS 4=SS */

    uint32_t slot;
    if (command(0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) || !slot || slot > X.max_slots) return -1;

    struct usb_dev *d = &devs[ndevs];
    memset(d, 0, sizeof *d);
    d->slot = (uint8_t)slot; d->speed = (uint8_t)speed; d->port = (uint8_t)port;
    d->in_ctx = dma_alloc_page(); d->out_ctx = dma_alloc_page(); d->buf = dma_alloc_page();
    if (!d->in_ctx || !d->out_ctx || !d->buf || ring_init(&d->ep0)) return -1;
    X.dcbaa[slot] = (uint64_t)d->out_ctx;

    unsigned mps0 = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    uint32_t *icc = ctx(d->in_ctx, 0), *sl = ctx(d->in_ctx, 1), *e0 = ctx(d->in_ctx, 2);
    icc[1] = 3;                                                 /* add: slot + EP0 */
    sl[0] = (speed << 20) | (1u << 27);
    sl[1] = port << 16;
    e0[1] = (3u << 1) | (4u << 3) | (mps0 << 16);               /* CErr=3, Control */
    e0[2] = (uint32_t)d->ep0.phys | 1;
    e0[3] = (uint32_t)(d->ep0.phys >> 32);
    e0[4] = 8;
    if (command((uint64_t)d->in_ctx, TRB_TYPE(TRB_ADDR_DEV) | (slot << 24), 0)) return -1;

    if (get_descriptor(d, 1, 8) || d->buf[1] != 1) return -1;   /* первые 8 байт: узнаём MPS EP0 */
    unsigned real = speed == 4 ? (1u << d->buf[7]) : d->buf[7];
    if (real && real != mps0) {
        memset(d->in_ctx, 0, PAGE);
        icc[1] = 2;
        e0[1] = (3u << 1) | (4u << 3) | (real << 16);
        if (command((uint64_t)d->in_ctx, TRB_TYPE(TRB_EVAL_CTX) | (slot << 24), 0)) return -1;
    }

    uint8_t iprod = 0;
    if (!get_descriptor(d, 1, 18) && d->buf[1] == 1) {          /* полный device descriptor, ошибки не критичны */
        struct usb_dev_info *in = &d->info;
        in->bcd_usb = (uint16_t)(d->buf[2] | d->buf[3] << 8);
        in->dev_class = d->buf[4]; in->dev_subclass = d->buf[5]; in->dev_proto = d->buf[6];
        in->vid = (uint16_t)(d->buf[8] | d->buf[9] << 8);
        in->pid = (uint16_t)(d->buf[10] | d->buf[11] << 8);
        iprod = d->buf[15];
    }

    if (get_descriptor(d, 2, 9)) return -1;                     /* конфигурация: сначала заголовок, потом всё */
    unsigned total = d->buf[2] | d->buf[3] << 8;
    if (total < 9 || total > 1024) return -1;
    if (get_descriptor(d, 2, (uint16_t)total)) return -1;

    unsigned before = nkbds;
    d->info.port = (uint8_t)port; d->info.slot = (uint8_t)slot; d->info.speed = (uint8_t)speed;
    parse_config(d, d->buf, total);

    if (iprod && !ctrl_xfer(d, 0x80, 6, (uint16_t)(3u << 8 | iprod), 0x0409, 64, 1) && d->buf[1] == 3) {
        unsigned n = d->buf[0] > 64 ? 64 : d->buf[0], o = 0;    /* строка Product: UTF-16LE -> ASCII */
        for (unsigned i = 2; i + 1 < n && o < sizeof d->info.product - 1; i += 2)
            d->info.product[o++] = d->buf[i + 1] ? '?' : (char)d->buf[i];
        d->info.product[o] = 0;
    }
    ndevs++;
    (void)before;
    {
        const struct usb_dev_info *in = &d->info;
        pr_info("port %u: slot %u, %04x:%04x, USB %x.%02x, %s, \"%s\"%s\n", port, (unsigned)slot,
                (unsigned)in->vid, (unsigned)in->pid, in->bcd_usb >> 8, in->bcd_usb & 0xFF,
                usb_speed_name(speed), in->product[0] ? in->product : "no name",
                in->is_kbd ? ", HID boot keyboard" : in->is_mouse ? ", HID boot mouse" : "");
        for (unsigned k = 0; k < in->nifaces && k < USB_INFO_IFACES; k++)
            pr_info("        if%u: class %02x/%02x/%02x (%s)\n", k, (unsigned)in->iface[k][0],
                    (unsigned)in->iface[k][1], (unsigned)in->iface[k][2], usb_class_name(in->iface[k][0]));
    }
    return 0;
}

/* ------------------------------------------------------------------ API для ядра (include/miga/usb.h) */
int usb_ctrl_get(struct usb_ctrl_info *out)
{
    if (!X.op) return -1;
    out->version = (uint16_t)X.ver; out->ports = (uint8_t)X.max_ports;
    out->slots = (uint8_t)X.max_slots; out->mmio = X.phys;
    return 0;
}

unsigned usb_dev_count(void) { return ndevs; }

int usb_dev_get(unsigned idx, struct usb_dev_info *out)
{
    if (idx >= ndevs) return -1;
    *out = devs[idx].info;
    return 0;
}

const char *usb_class_name(unsigned c)
{
    switch (c) {
    case 0x00: return "per-interface"; case 0x01: return "audio";   case 0x02: return "cdc";
    case 0x03: return "hid";           case 0x07: return "printer"; case 0x08: return "mass-storage";
    case 0x09: return "hub";           case 0x0A: return "cdc-data"; case 0x0E: return "video";
    case 0xE0: return "wireless";      case 0xFF: return "vendor";
    }
    return "other";
}

const char *usb_speed_name(unsigned s)
{
    return s == 4 ? "SuperSpeed" : s == 3 ? "HighSpeed" : s == 2 ? "LowSpeed" : s == 1 ? "FullSpeed" : "?";
}

/* ------------------------------------------------------------------ API для usbhid */
unsigned xhci_hid_count(void) { return nkbds; }
unsigned xhci_hid_kind(unsigned idx) { return idx < nkbds ? kbds[idx].kind : 0; }

int xhci_hid_start(unsigned idx, usb_hid_cb cb)
{
    if (idx >= nkbds) return -1;
    struct usb_kbd *k = &kbds[idx];
    struct usb_dev *d = k->dev;

    k->rep = dma_alloc_page();
    if (!k->rep || ring_init(&k->ring)) return -1;
    k->dci = (uint8_t)(k->ep * 2 + 1);

    memset(d->in_ctx, 0, PAGE);
    uint32_t *icc = ctx(d->in_ctx, 0), *sl = ctx(d->in_ctx, 1), *e = ctx(d->in_ctx, k->dci + 1u);
    icc[1] = 1u | (1u << k->dci);
    memcpy(sl, ctx(d->out_ctx, 1), X.ctx_size);                 /* slot context берём из текущего */
    sl[0] = (sl[0] & 0x07FFFFFFu) | ((uint32_t)k->dci << 27);
    e[0] = ep_interval(d->speed, k->interval) << 16;
    e[1] = (3u << 1) | (7u << 3) | ((uint32_t)k->mps << 16);    /* CErr=3, Interrupt IN */
    e[2] = (uint32_t)k->ring.phys | 1;
    e[3] = (uint32_t)(k->ring.phys >> 32);
    e[4] = (uint32_t)k->rlen | ((uint32_t)k->rlen << 16);
    if (command((uint64_t)d->in_ctx, TRB_TYPE(TRB_CFG_EP) | ((uint32_t)d->slot << 24), 0)) return -1;

    if (ctrl_xfer(d, 0x00, 9, k->cfg, 0, 0, 0)) return -1;      /* SET_CONFIGURATION */
    ctrl_xfer(d, 0x21, 0x0B, 0, k->iface, 0, 0);                /* HID SET_PROTOCOL(boot), ошибки не критичны */

    k->cb = cb;
    k->active = 1;
    kbd_queue(k);
    return 0;
}

/* ------------------------------------------------------------------ probe */
static int xhci_probe(const struct boot_info *bi)
{
    (void)bi;
    uint8_t bus, dev, fn;
    if (pci_find_class(0x0C0330, &bus, &dev, &fn)) return -1;   /* class 0C, subclass 03, prog-if 30 = xHCI */

    pci_write32(bus, dev, fn, PCI_CMD, pci_read32(bus, dev, fn, PCI_CMD) | 0x406); /* MEM + bus master + INTx off */
    uint32_t bar = pci_read32(bus, dev, fn, PCI_BAR0);
    if (bar & 1) return -2;
    uint64_t phys = bar & ~0xFull;
    if (((bar >> 1) & 3) == 2) phys |= (uint64_t)pci_read32(bus, dev, fn, PCI_BAR0 + 4) << 32;
    if (!phys) return -3;

    uint8_t *cap = mmio_map(phys, 0x20000);
    if (!cap) { pr_warn("cannot map xHCI BAR %#lx\n", phys); return -4; }

    unsigned caplen = r32(cap, 0) & 0xFF, ver = r32(cap, 0) >> 16;
    X.ver = ver; X.phys = phys;
    uint32_t hcs1 = r32(cap, 4), hcs2 = r32(cap, 8), hcc1 = r32(cap, 0x10);
    X.cap = cap;
    X.op = cap + caplen;
    X.rt = cap + (r32(cap, 0x18) & ~0x1Fu);
    X.db = (uint32_t *)(cap + (r32(cap, 0x14) & ~3u));
    X.ctx_size = (hcc1 & 4) ? 64 : 32;
    X.max_slots = hcs1 & 0xFF;
    X.max_ports = hcs1 >> 24;
    if (X.max_slots > 64) X.max_slots = 64;

    /* забрать контроллер у BIOS (USB Legacy Support, extended capability id=1) */
    for (unsigned off = ((hcc1 >> 16) & 0xFFFF) * 4, n = 0; off && n < 32; n++) {
        uint32_t v = r32(cap, off);
        if ((v & 0xFF) == 1) {
            w32(cap, off, v | (1u << 24));
            wait_reg(cap, off, 1u << 16, 0, 1000);
        }
        unsigned next = (v >> 8) & 0xFF;
        if (!next) break;
        off += next * 4;
    }

    if (!(r32(X.op, 4) & 1)) { w32(X.op, 0, r32(X.op, 0) & ~1u); wait_reg(X.op, 4, 1, 1, 100); } /* stop */
    w32(X.op, 0, 2);                                                                              /* HCRST */
    if (wait_reg(X.op, 0, 2, 0, 500) || wait_reg(X.op, 4, 1u << 11, 0, 500)) return -5;          /* ждём CNR=0 */

    w32(X.op, 0x38, X.max_slots);                                /* CONFIG */

    X.dcbaa = dma_alloc_page();
    if (!X.dcbaa) return -6;
    unsigned sp = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);   /* scratchpad buffers */
    if (sp) {
        uint64_t *arr = dma_alloc_page();
        if (!arr || sp > 512) return -6;
        for (unsigned i = 0; i < sp; i++) {
            void *pg = dma_alloc_page();
            if (!pg) return -6;
            arr[i] = (uint64_t)pg;
        }
        X.dcbaa[0] = (uint64_t)arr;
    }
    w64(X.op, 0x30, (uint64_t)X.dcbaa);

    if (ring_init(&X.cmd)) return -6;
    w64(X.op, 0x18, X.cmd.phys | 1);                             /* CRCR: кольцо команд + RCS */

    X.ev = dma_alloc_page();
    uint64_t *erst = dma_alloc_page();
    if (!X.ev || !erst) return -6;
    X.ev_phys = (uint64_t)X.ev;
    X.ev_idx = 0; X.ev_ccs = 1;
    erst[0] = X.ev_phys;
    erst[1] = RING_TRBS;
    w32(X.rt, 0x28, 1);                                          /* ERSTSZ */
    w64(X.rt, 0x38, X.ev_phys);                                  /* ERDP */
    w64(X.rt, 0x30, (uint64_t)erst);                             /* ERSTBA (последним) */

    w32(X.op, 0, 1);                                             /* RUN */
    if (wait_reg(X.op, 4, 1, 0, 500)) return -7;                 /* HCH=0 */

    udelay(100000);                                              /* дать портам обнаружить устройства */
    for (unsigned p = 1; p <= X.max_ports; p++) enum_port(p);

    pr_info("xHCI %x.%02x at %#lx: %u ports, %u slots, %u device(s), %u HID boot device(s)\n",
            ver >> 8, ver & 0xFF, phys, X.max_ports, X.max_slots, ndevs, nkbds);
    return 0;
}

MIGA_DRIVER(xhci, "xhci", DRV_VER(1, 0), DRV_NORMAL, xhci_probe);

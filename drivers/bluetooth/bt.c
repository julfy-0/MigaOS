/* Bluetooth: адаптеры подключены к USB (класс E0h/01h/01h — Wireless Controller / RF / Bluetooth).
 * Берём их из списка устройств xHCI-драйвера; HCI-обмена (bulk/interrupt endpoints, прошивка) нет. */
#define MIGA_TAG "bt"
#include <miga/driver.h>
#include <miga/usb.h>
#include <miga/log.h>
#include <miga/string.h>
#include <miga/bt.h>

static struct bt_info bts[BT_MAX];
static unsigned nbt;

static const struct { uint16_t vid; const char *name; } vendors[] = {
    {0x8087, "Intel"}, {0x0BDA, "Realtek"}, {0x0CF3, "Qualcomm Atheros"}, {0x0E8D, "MediaTek"},
    {0x0A5C, "Broadcom"}, {0x0A12, "Cambridge Silicon Radio"}, {0x13D3, "IMC Networks"}, {0x04CA, "Lite-On"},
    {0x0489, "Foxconn"}, {0x0930, "Toshiba"}, {0x1286, "Marvell"}, {0x413C, "Dell"},
};

unsigned bt_count(void) { return nbt; }

int bt_get(unsigned idx, struct bt_info *out)
{
    if (idx >= nbt) return -1;
    *out = bts[idx];
    return 0;
}

static void print_one(const struct bt_info *b, int tagged)
{
    kprintf("%sBluetooth USB port %u: %04x:%04x USB %x.%02x %s  %s %s\n", tagged ? "[bt] " : "", (unsigned)b->port,
            (unsigned)b->vid, (unsigned)b->pid, b->bcd_usb >> 8, b->bcd_usb & 0xFF, usb_speed_name(b->speed),
            b->vendor_name, b->name[0] ? b->name : "(no name)");
}

void bt_print_all(void)
{
    if (!nbt) { kprintf("bt: no USB Bluetooth adapters found\n"); return; }
    for (unsigned i = 0; i < nbt; i++) print_one(&bts[i], 0);
}

static int bt_probe(const struct boot_info *bi)
{
    (void)bi;
    for (unsigned i = 0; i < usb_dev_count() && nbt < BT_MAX; i++) {
        struct usb_dev_info d;
        if (usb_dev_get(i, &d)) continue;
        int is_bt = d.dev_class == 0xE0 && d.dev_subclass == 1 && d.dev_proto == 1;
        for (unsigned k = 0; k < d.nifaces && k < USB_INFO_IFACES; k++)
            if (d.iface[k][0] == 0xE0 && d.iface[k][1] == 1 && d.iface[k][2] == 1) is_bt = 1;
        if (!is_bt) continue;

        struct bt_info *b = &bts[nbt++];
        memset(b, 0, sizeof *b);
        b->port = d.port; b->speed = d.speed; b->vid = d.vid; b->pid = d.pid; b->bcd_usb = d.bcd_usb;
        const char *vn = "unknown vendor";
        for (unsigned v = 0; v < sizeof vendors / sizeof *vendors; v++)
            if (vendors[v].vid == d.vid) { vn = vendors[v].name; break; }
        unsigned o = 0;
        for (; vn[o] && o < sizeof b->vendor_name - 1; o++) b->vendor_name[o] = vn[o];
        for (o = 0; d.product[o] && o < sizeof b->name - 1; o++) b->name[o] = d.product[o];
        print_one(b, 1);
    }
    return nbt ? 0 : -1;
}

MIGA_DRIVER(bluetooth, "bluetooth", DRV_VER(0, 1), DRV_LATE, bt_probe);   /* после xhci: ему нужен список USB-устройств */

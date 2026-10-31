/* Приложение 4: Devices — список найденного железа: USB, диски, сеть, видео, звук/Bluetooth.
 * Данные берёт из публичных интерфейсов драйверов (miga/usb.h, disk.h, net.h, gpu.h, audio.h, bt.h). */
#include <kernel/gui.h>
#include <miga/string.h>
#include <miga/usb.h>
#include <miga/disk.h>
#include <miga/net.h>
#include <miga/gpu.h>
#include <miga/audio.h>
#include <miga/bt.h>

#define MAXL  96
#define LW    58
#define SHOWN 14
#define NTABS 5

static const char *const tabs[NTABS] = {"USB", "Disks", "Network", "GPU", "Audio/BT"};
static char lines[MAXL][LW];
static unsigned nl, scroll;
static int tab;

static void add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void add(const char *fmt, ...)
{
    if (nl >= MAXL) return;
    va_list ap;
    va_start(ap, fmt);
    gui_vsnprintf(lines[nl++], LW, fmt, ap);
    va_end(ap);
}

static void build(void)
{
    nl = 0;
    switch (tab) {
    case 0: {
        struct usb_ctrl_info c;
        if (usb_ctrl_get(&c)) { add("No xHCI controller found"); break; }
        add("xHCI %x.%02x  ports %u  slots %u  devices %u", c.version >> 8, c.version & 0xFF, c.ports, c.slots, usb_dev_count());
        for (unsigned i = 0; i < usb_dev_count(); i++) {
            struct usb_dev_info d;
            if (usb_dev_get(i, &d)) continue;
            add(" ");
            add("Port %u  %04x:%04x  USB %x.%02x  %s", d.port, d.vid, d.pid, d.bcd_usb >> 8, d.bcd_usb & 0xFF, usb_speed_name(d.speed));
            add("  %s%s", d.product[0] ? d.product : "(no name)", d.is_kbd ? "  [keyboard]" : "");
            add("  class %02x (%s)", d.dev_class, usb_class_name(d.dev_class));
        }
        break; }
    case 1:
        for (unsigned i = 0; i < disk_count(); i++) {
            struct disk_info d;
            if (disk_get(i, &d)) continue;
            if (i) add(" ");
            add("[%u] %s  %s", i, d.bus, disk_kind_name(d.kind));
            add("  %s", d.model);
            if (d.sectors) add("  %lu GiB   fw %s", (d.sectors * d.sector_size) >> 30, d.firmware);
            else add("  size unknown   fw %s", d.firmware);
            add("  sn %s", d.serial);
        }
        if (!nl) add("No disks found");
        break;
    case 2:
        for (unsigned i = 0; i < net_count(); i++) {
            struct net_info n;
            if (net_get(i, &n)) continue;
            static const char *const lk[] = {"unknown", "down", "up"};
            if (i) add(" ");
            add("%s  %s", n.vendor_name, n.name);
            add("  %s  link %s  %u Mbps", n.kind == NET_WIFI ? "Wi-Fi" : "Ethernet", lk[n.link % 3], n.speed_mbps);
            if (n.has_mac)
                add("  MAC %02x:%02x:%02x:%02x:%02x:%02x", n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
        }
        if (!nl) add("No network adapters found");
        break;
    case 3:
        for (unsigned i = 0; i < gpu_count(); i++) {
            struct gpu_info g;
            if (gpu_get(i, &g)) continue;
            if (i) add(" ");
            add("%s  %04x:%04x  %s", g.vendor_name, g.vendor, g.device, g.integrated ? "iGPU" : "dGPU");
            add("  %s%s", g.family, g.boot_fb ? "  [boot fb]" : "");
            add("  VRAM aperture %lu MiB", g.vram_aperture >> 20);
            if (g.link_width) add("  PCIe %s GT/s x%u", gpu_link_gts(g.link_speed), g.link_width);
        }
        if (!nl) add("No display adapters found");
        break;
    default:
        for (unsigned i = 0; i < audio_count(); i++) {
            struct audio_info a;
            if (audio_get(i, &a)) continue;
            add("HD Audio %u.%u  %04x:%04x  out %u  in %u", a.vmaj, a.vmin, a.vendor, a.device, a.oss, a.iss);
            for (unsigned k = 0; k < a.ncodecs && k < AUDIO_CODECS; k++)
                add("  codec %u: %04x:%04x", a.codec[k].addr, a.codec[k].vendor, a.codec[k].device);
        }
        if (!nl) add("No audio controllers found");
        add(" ");
        for (unsigned i = 0; i < bt_count(); i++) {
            struct bt_info b;
            if (bt_get(i, &b)) continue;
            add("Bluetooth: %s %s (%04x:%04x)", b.vendor_name, b.name, b.vid, b.pid);
        }
        if (!bt_count()) add("No Bluetooth adapters found");
        break;
    }
    if (scroll + SHOWN > nl) scroll = nl > SHOWN ? nl - SHOWN : 0;
}

static void draw(struct gui_win *w)
{
    build();
    gui_clear(w, GUI_C_WINBG);
    int x = 8;
    for (int i = 0; i < NTABS; i++) {
        int tw = gui_text_width(tabs[i]) + 24;
        gui_fill(w, x, 6, tw, 24, i == tab ? GUI_C_ACCENT : GUI_C_PANEL2);
        char t[24];
        gui_snprintf(t, sizeof t, "%d %s", i + 1, tabs[i]);
        gui_text(w, x + 6, 10, t, 0xFFFFFF);
        x += tw + 16;
    }
    gui_fill(w, 4, 38, w->w - 8, SHOWN * GUI_CHAR_H + 10, GUI_C_PANEL);
    gui_rect(w, 4, 38, w->w - 8, SHOWN * GUI_CHAR_H + 10, GUI_C_LINE);
    for (unsigned i = 0; i < SHOWN && scroll + i < nl; i++) {
        const char *l = lines[scroll + i];
        gui_text(w, 10, 43 + (int)i * GUI_CHAR_H, l, l[0] == ' ' ? GUI_C_DIM : GUI_C_TEXT);
    }
    gui_printf(w, 8, w->h - 22, GUI_C_DIM, "1-5 / Left Right: tab   Up Down: scroll   (%u lines)", nl);
    gui_present(w);
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type != GUI_EV_KEY) return;
    int k = e->key;
    if (k >= '1' && k < '1' + NTABS) { tab = k - '1'; scroll = 0; }
    else if (k == GUI_KEY_RIGHT) { tab = (tab + 1) % NTABS; scroll = 0; }
    else if (k == GUI_KEY_LEFT)  { tab = (tab + NTABS - 1) % NTABS; scroll = 0; }
    else if (k == GUI_KEY_DOWN)  { if (scroll + SHOWN < nl) scroll++; }
    else if (k == GUI_KEY_UP)    { if (scroll) scroll--; }
    else if (k == 'r' || k == 'R') { /* перерисовать/пересобрать список */ }
    else return;
    draw(w);
}

static void start(struct gui_win *w) { tab = 0; scroll = 0; draw(w); }

const struct gui_app app_devices = {
    .name = "Devices", .glyph = "Dv", .color = 0x1E9E8A, .w = 440, .h = 310, .tick_ms = 0,
    .start = start, .event = ev,
};

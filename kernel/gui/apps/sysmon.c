/* Приложение 1: System Monitor — аптайм, память с графиком, CPU (по клавише R), число устройств. */
#include <kernel/gui.h>
#include <kernel/kernel.h>
#include <kernel/sysinfo.h>
#include <miga/time.h>
#include <miga/string.h>
#include <miga/usb.h>
#include <miga/disk.h>
#include <miga/net.h>
#include <miga/gpu.h>
#include <miga/audio.h>
#include <miga/bt.h>

#define HIST 60
static struct {
    uint32_t free_mib[HIST];   /* история свободной памяти, МиБ, по одной точке в секунду */
    unsigned n;
    struct cpu_live live;
    int have_live;
} S;

static void sample(void)
{
    uint32_t v = (uint32_t)(pmm_free_pages() >> 8);          /* страницы по 4 КиБ -> МиБ */
    if (S.n == HIST) { memmove(S.free_mib, S.free_mib + 1, (HIST - 1) * sizeof S.free_mib[0]); S.n--; }
    S.free_mib[S.n++] = v;
}

static void draw(struct gui_win *w)
{
    char b[64];
    gui_clear(w, GUI_C_WINBG);
    gui_text(w, 12, 8, "System Monitor", GUI_C_TEXT);
    gui_line(w, 12, 28, w->w - 12, 28, GUI_C_LINE);

    uint64_t ms = time_uptime_ms(), s = ms / 1000;
    gui_printf(w, 12, 38, GUI_C_DIM, "Uptime");
    gui_printf(w, 120, 38, GUI_C_TEXT, "%u:%02u:%02u   (%lu ticks)", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60), time_ticks());

    uint64_t tot = pmm_total_pages() >> 8, fr = pmm_free_pages() >> 8, used = tot - fr;
    gui_printf(w, 12, 60, GUI_C_DIM, "Memory");
    gui_snprintf(b, sizeof b, "%lu / %lu MiB used", used, tot);
    gui_text(w, 120, 60, b, GUI_C_TEXT);
    gui_bar(w, 12, 80, w->w - 24, 14, used, tot ? tot : 1, tot && used * 100 / tot > 85 ? GUI_C_BAD : GUI_C_ACCENT, GUI_C_PANEL);

    /* график свободной памяти за последние 60 с */
    int gx = 12, gy = 106, gw = w->w - 24, gh = 70;
    gui_fill(w, gx, gy, gw, gh, GUI_C_PANEL);
    gui_rect(w, gx, gy, gw, gh, GUI_C_LINE);
    gui_text(w, gx + 6, gy + 4, "free MiB, last 60 s", GUI_C_DIM);
    if (S.n > 1) {
        uint32_t lo = S.free_mib[0], hi = S.free_mib[0];
        for (unsigned i = 1; i < S.n; i++) { if (S.free_mib[i] < lo) lo = S.free_mib[i]; if (S.free_mib[i] > hi) hi = S.free_mib[i]; }
        if (hi == lo) hi = lo + 1;
        int py = 0, px = 0;
        for (unsigned i = 0; i < S.n; i++) {
            int x = gx + 2 + (int)(i * (unsigned)(gw - 4) / (HIST - 1));
            int y = gy + gh - 4 - (int)((S.free_mib[i] - lo) * (unsigned)(gh - 28) / (hi - lo));
            if (i) gui_line(w, px, py, x, y, GUI_C_GOOD);
            px = x; py = y;
        }
    }

    gui_printf(w, 12, 188, GUI_C_DIM, "CPU");
    if (S.have_live) {
        gui_printf(w, 120, 188, GUI_C_TEXT, "TSC %u MHz", S.live.tsc_mhz);
        if (S.live.eff_mhz) gui_printf(w, 120, 206, GUI_C_TEXT, "effective ~%u MHz", S.live.eff_mhz);
        else gui_printf(w, 120, 206, GUI_C_DIM, "effective: n/a (VM)");
        if (S.live.temp_c > -1000) gui_printf(w, 120, 224, S.live.throttled ? GUI_C_BAD : GUI_C_TEXT, "%d C (Tjmax %u)", S.live.temp_c, S.live.tjmax);
        else gui_printf(w, 120, 224, GUI_C_DIM, "temperature: n/a");
    } else {
        gui_text(w, 120, 188, "press R to measure (0.1 s)", GUI_C_DIM);
    }

    gui_line(w, 12, 250, w->w - 12, 250, GUI_C_LINE);
    gui_printf(w, 12, 258, GUI_C_TEXT, "USB %u", usb_dev_count());
    gui_printf(w, 100, 258, GUI_C_TEXT, "Disks %u", disk_count());
    gui_printf(w, 200, 258, GUI_C_TEXT, "Net %u", net_count());
    gui_printf(w, 12, 278, GUI_C_TEXT, "GPU %u", gpu_count());
    gui_printf(w, 100, 278, GUI_C_TEXT, "Audio %u", audio_count());
    gui_printf(w, 200, 278, GUI_C_TEXT, "BT %u", bt_count());
    gui_present(w);
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type == GUI_EV_TICK) { sample(); draw(w); }
    else if (e->type == GUI_EV_KEY && (e->key == 'r' || e->key == 'R')) {
        S.have_live = sysinfo_cpu_live(&S.live) == 0;
        draw(w);
    }
}

static void start(struct gui_win *w)
{
    memset(&S, 0, sizeof S);
    sample();
    draw(w);
}

const struct gui_app app_sysmon = {
    .name = "System Monitor", .glyph = "SM", .color = 0x2F6FDE, .w = 360, .h = 304, .tick_ms = 1000,
    .start = start, .event = ev,
};

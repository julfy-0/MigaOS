/* Display Test: что заявляет монитор (EDID), что рисует UI, бегущая полоса для проверки тиринга,
 * экспериментальная проба VGA-vblank (работает только там, где есть legacy VGA, на многих GPU вернёт n/a). */
#include <kernel/gui.h>
#include <kernel/display.h>
#include <miga/time.h>
#include <miga/io.h>

#define BG 0x0B0F1A
#define TY 140
#define TH 140
#define BARW 14
#define SPEED 600                          /* пикселей в секунду */

static const unsigned presets[5] = {60, 75, 120, 144, 240};
static int oldx; static unsigned probe_hz; static int probed; static uint32_t last_info;

static unsigned probe_vblank(void)
{
    uint64_t t0 = time_now_us(), edges = 0;
    int prev = (inb(0x3DA) >> 3) & 1;
    while (time_now_us() - t0 < 500000) {
        int v = (inb(0x3DA) >> 3) & 1;
        if (v && !prev) edges++;
        prev = v;
    }
    return (unsigned)(edges * 2);
}

static void info(struct gui_win *w)
{
    gui_fill(w, 0, 0, w->w, TY - 4, BG);
    gui_text_big(w, 12, 8, "Display Test", GUI_C_TEXT, 2);
    gui_printf(w, 12, 44, GUI_C_TEXT, "Reported (EDID) refresh : %u Hz", display_refresh_hz());
    gui_printf(w, 12, 62, GUI_C_TEXT, "UI frame target         : %u Hz", gui_target_hz());
    gui_printf(w, 12, 80, GUI_C_GOOD, "UI frames pushed / sec  : %u", gui_fps());
    if (!probed) gui_text(w, 12, 98, "VGA vblank probe        : press P", GUI_C_DIM);
    else if (probe_hz >= 20 && probe_hz <= 500) gui_printf(w, 12, 98, GUI_C_WARN, "VGA vblank probe        : ~%u Hz", probe_hz);
    else gui_text(w, 12, 98, "VGA vblank probe        : n/a (no legacy VGA retrace)", GUI_C_DIM);
    gui_text(w, 12, 116, "1-5 = target 60/75/120/144/240   P = probe   slanted bar = tearing", GUI_C_DIM);
    gui_damage(w, 0, 0, w->w, TY - 4);
}

static int barx(const struct gui_win *w, uint32_t now)
{
    int span = w->w - BARW;
    int x = (int)(((uint64_t)now * SPEED / 1000) % (uint64_t)(2 * span));
    return x > span ? 2 * span - x : x;
}

static void start(struct gui_win *w)
{
    gui_clear(w, BG); oldx = 0; probed = 0; last_info = 0;
    info(w);
    gui_text(w, 12, TY + TH + 4, "Real mode is set by firmware (GOP); the OS can't read or change it.", GUI_C_DIM);
    gui_present(w);
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type == GUI_EV_TICK) {
        int x = barx(w, e->arg);
        if (x != oldx) {
            gui_fill(w, oldx, TY, BARW, TH, BG);
            gui_fill(w, x, TY, BARW, TH, 0xFFFFFF);
            int a = x < oldx ? x : oldx, b = (x > oldx ? x : oldx) + BARW;
            gui_damage(w, a, TY, b - a, TH);
            oldx = x;
        }
        if (e->arg - last_info >= 500) { last_info = e->arg; info(w); }
        return;
    }
    if (e->type != GUI_EV_KEY) return;
    char k = (char)e->key;
    if (k >= '1' && k <= '5') { display_set_refresh_hz(presets[k - '1']); gui_set_target_hz(0); info(w); }
    else if (k == 'p' || k == 'P') { probe_hz = probe_vblank(); probed = 1; info(w); }
}

const struct gui_app app_dispcheck = {
    .name = "Display Test", .glyph = "Hz", .color = 0x2FBFBF, .w = 560, .h = 300, .tick_ms = 2,
    .start = start, .event = ev,
};

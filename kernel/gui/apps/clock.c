/* Приложение 12: Clock — аналоговые и цифровые часы, дата, мини-календарь, секундомер.
 * Время берётся из RTC (gui_get_time) с учётом настройки sys.tz. Клавиши: S — старт/стоп секундомера, R — сброс. */
#include <kernel/gui.h>
#include <miga/string.h>

#define WIN_W 340
#define WIN_H 400
#define CX    (WIN_W / 2)
#define CY    130
#define RAD   100

static const char *const WD[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const MO[12] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December"};
/* round(1000 * sin(6 deg * k)), k = 0..15; остальное зеркалированием */
static const int S60[16] = {0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000};

static int sin60(int k)                         /* k = 0..59: sin(k * 6 deg) * 1000 */
{
    k %= 60;
    int neg = k >= 30;
    if (neg) k -= 30;
    int v = k <= 15 ? S60[k] : S60[30 - k];
    return neg ? -v : v;
}
static int cos60(int k) { return sin60(k + 15); }

static int sw_run;
static uint32_t sw_acc, sw_start;               /* накопленное время и момент старта, мс */

static void hand(struct gui_win *w, int pos60, int len, uint32_t c, int thick)
{
    int dx = sin60(pos60) * len / 1000, dy = -cos60(pos60) * len / 1000;
    for (int t = -(thick / 2); t <= thick / 2; t++) {
        gui_line(w, CX + t, CY, CX + dx + t, CY + dy, c);
        gui_line(w, CX, CY + t, CX + dx, CY + dy + t, c);
    }
}

static uint32_t sw_ms(uint32_t now) { return sw_acc + (sw_run ? now - sw_start : 0); }

static void draw(struct gui_win *w, uint32_t now)
{
    struct gui_time t;
    gui_get_time(&t);
    gui_clear(w, GUI_C_WINBG);

    gui_disc(w, CX, CY, RAD + 8, 0x252E47);
    gui_disc(w, CX, CY, RAD + 5, 0x0F1424);
    for (int k = 0; k < 60; k++) {
        int big = k % 5 == 0, r0 = RAD - (big ? 12 : 6), r1 = RAD - 1;
        int sx = sin60(k), cy = cos60(k);
        gui_line(w, CX + sx * r0 / 1000, CY - cy * r0 / 1000, CX + sx * r1 / 1000, CY - cy * r1 / 1000,
                 big ? GUI_C_TEXT : GUI_C_LINE);
    }
    for (int h = 1; h <= 12; h++) {
        char s[4];
        gui_snprintf(s, sizeof s, "%d", h);
        int px = CX + sin60(h * 5) * (RAD - 26) / 1000, py = CY - cos60(h * 5) * (RAD - 26) / 1000;
        gui_text(w, px - gui_text_width(s) / 2, py - 8, s, GUI_C_DIM);
    }
    hand(w, (t.hour % 12) * 5 + t.min / 12, RAD - 48, GUI_C_TEXT, 3);
    hand(w, t.min, RAD - 22, GUI_C_ACCENT, 3);
    hand(w, t.sec, RAD - 10, GUI_C_BAD, 1);
    gui_disc(w, CX, CY, 6, GUI_C_ACCENT);
    gui_disc(w, CX, CY, 2, GUI_C_WINBG);

    char s[48];
    gui_snprintf(s, sizeof s, "%02d:%02d:%02d", t.hour, t.min, t.sec);
    gui_text_big(w, (w->w - gui_text_width(s) * 2) / 2, 252, s, 0xFFFFFF, 2);
    gui_snprintf(s, sizeof s, "%s, %d %s %d", WD[t.wday], t.day, MO[t.mon - 1], t.year);
    gui_text(w, (w->w - gui_text_width(s)) / 2, 288, s, GUI_C_DIM);

    gui_line(w, 16, 316, w->w - 16, 316, GUI_C_LINE);
    uint32_t ms = sw_ms(now);
    gui_printf(w, 16, 332, GUI_C_TEXT, "Stopwatch  %02u:%02u.%02u", ms / 60000, ms / 1000 % 60, ms / 10 % 100);
    gui_text(w, 16, 356, sw_run ? "S: stop    R: reset" : "S: start   R: reset", GUI_C_DIM);
    gui_text(w, 16, 376, "Time zone: setting sys.tz (hours)", GUI_C_DIM);
    gui_present(w);
}

static uint32_t last_now;                        /* uptime из последнего TICK (клавиши времени не несут) */

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type == GUI_EV_TICK) { last_now = e->arg; draw(w, last_now); return; }
    if (e->type != GUI_EV_KEY) return;
    if (e->key == 's' || e->key == 'S') {
        if (sw_run) { sw_acc = sw_ms(last_now); sw_run = 0; }
        else { sw_start = last_now; sw_run = 1; }
    } else if (e->key == 'r' || e->key == 'R') { sw_run = 0; sw_acc = 0; }
    draw(w, last_now);
}
static void start(struct gui_win *w) { sw_run = 0; sw_acc = 0; last_now = 0; draw(w, 0); }

const struct gui_app app_clock = {
    .name = "Clock", .glyph = "Ck", .color = 0xF2B84B, .w = WIN_W, .h = WIN_H, .tick_ms = 100,
    .start = start, .event = ev,
};

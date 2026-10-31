/* Приложение 11: Settings — редактор реестра настроек (kernel/core/settings.c).
 * bool переключается Enter/пробелом или Left/Right, int — Left/Right (±1) и +/- (±10), текст — Enter, ввод, Enter.
 * Значения живут до перезагрузки (сохранять некуда: нет записи на диск). Esc сервер забирает себе, поэтому
 * отмена ввода текста — Ctrl+C. */
#include <kernel/gui.h>
#include <kernel/settings.h>
#include <miga/string.h>

#define WIN_W   560
#define WIN_H   360
#define ROW_H   36
#define LIST_Y  40
#define FOOT_H  64
#define VIS     ((WIN_H - LIST_Y - FOOT_H) / ROW_H)

static int sel, topi, editing, elen;
static char ebuf[SET_VAL_MAX];
static char msg[72];
static uint32_t msg_col;

static void say(uint32_t c, const char *a, const char *b)
{
    gui_snprintf(msg, sizeof msg, "%s%s", a, b);
    msg_col = c;
}

static void scroll_to_sel(void)
{
    if (sel < topi) topi = sel;
    if (sel >= topi + VIS) topi = sel - VIS + 1;
    if (topi < 0) topi = 0;
}

static void apply(const struct setting_info *s, const char *val)
{
    int r = settings_set(s->key, val);
    if (r == SET_OK) { say(GUI_C_GOOD, s->key, " updated"); }
    else say(GUI_C_BAD, s->key, s->type == SET_STR ? ": use a-z 0-9 . _ - (max 23)" : ": value rejected");
}

static void step(int dir, int big)
{
    struct setting_info s;
    if (settings_info((unsigned)sel, &s)) return;
    if (s.type == SET_BOOL) {
        apply(&s, !strcmp(s.value, "on") ? "off" : "on");
    } else if (s.type == SET_INT) {
        int v = settings_get_int(s.key, 0) + dir * (big ? 10 : 1);
        if (v < s.min) v = s.min;
        if (v > s.max) v = s.max;
        if (settings_set_int(s.key, v)) say(GUI_C_BAD, s.key, ": value rejected");
        else say(GUI_C_GOOD, s.key, " updated");
    }
}

static void activate(void)
{
    struct setting_info s;
    if (settings_info((unsigned)sel, &s)) return;
    if (s.type == SET_STR) { editing = 1; elen = 0; ebuf[0] = 0; msg[0] = 0; }
    else step(1, 0);                                       /* bool переключается, int +1 */
}

static void draw(struct gui_win *w)
{
    struct setting_info s;
    gui_clear(w, GUI_C_WINBG);
    gui_fill(w, 0, 0, w->w, 32, GUI_C_PANEL);
    gui_text(w, 12, 8, "Settings", GUI_C_TEXT);
    gui_printf(w, w->w - 190, 8, GUI_C_DIM, "%u items (until reboot)", settings_count());

    for (int i = 0; i < VIS && topi + i < (int)settings_count(); i++) {
        int idx = topi + i, y = LIST_Y + i * ROW_H;
        if (settings_info((unsigned)idx, &s)) continue;
        if (idx == sel) gui_fill(w, 4, y, w->w - 8, ROW_H - 2, GUI_C_PANEL2);
        gui_text(w, 14, y + 3, s.key, GUI_C_TEXT);
        gui_text(w, 14, y + 19, s.help, GUI_C_DIM);

        int bx = w->w - 170, by = y + 5;
        int ed = editing && idx == sel;
        gui_fill(w, bx, by, 150, 24, ed ? 0x0B0F1A : GUI_C_PANEL);
        gui_rect(w, bx, by, 150, 24, ed ? GUI_C_ACCENT : GUI_C_LINE);
        if (ed) {
            gui_text(w, bx + 6, by + 4, ebuf, GUI_C_TEXT);
            gui_fill(w, bx + 6 + elen * GUI_CHAR_W, by + 18, GUI_CHAR_W, 2, GUI_C_GOOD);
        } else if (s.type == SET_BOOL) {
            int on = !strcmp(s.value, "on");
            gui_fill(w, bx + 6, by + 5, 34, 14, on ? GUI_C_GOOD : GUI_C_LINE);
            gui_fill(w, bx + (on ? 24 : 8), by + 7, 14, 10, 0xFFFFFF);
            gui_text(w, bx + 50, by + 4, on ? "ON" : "OFF", on ? GUI_C_GOOD : GUI_C_DIM);
        } else if (s.type == SET_INT) {
            gui_printf(w, bx + 6, by + 4, GUI_C_TEXT, "%s", s.value);
            gui_printf(w, bx + 150 - 6 - 8 * 9, by + 4, GUI_C_DIM, "%d..%d", (int)s.min, (int)s.max);
        } else {
            gui_text(w, bx + 6, by + 4, s.value, GUI_C_TEXT);
        }
        if (strcmp(s.value, s.def)) gui_text(w, bx - 14, by + 4, "*", GUI_C_WARN);
    }

    int fy = WIN_H - FOOT_H;
    gui_line(w, 0, fy, w->w, fy, GUI_C_LINE);
    if (msg[0]) gui_text(w, 8, fy + 4, msg, msg_col);
    else gui_text(w, 8, fy + 4, "* marks a value that differs from the default", GUI_C_DIM);
    if (editing) {
        gui_text(w, 8, fy + 24, "Type the new text, Enter: apply, Ctrl+U: clear, Ctrl+C: cancel", GUI_C_DIM);
    } else {
        gui_text(w, 8, fy + 24, "Up/Down: select   Left/Right: change   Enter: toggle/edit", GUI_C_DIM);
        gui_text(w, 8, fy + 42, "+/-: step 10   D: default   Shift+R: reset all", GUI_C_DIM);
    }
    gui_present(w);
}

static void edit_key(int k)
{
    struct setting_info s;
    if (k == '\n') {
        editing = 0;
        if (elen && !settings_info((unsigned)sel, &s)) apply(&s, ebuf);
    } else if (k == 0x03) { editing = 0; say(GUI_C_DIM, "cancelled", ""); }
    else if (k == 0x15) { elen = 0; ebuf[0] = 0; }
    else if (k == '\b') { if (elen) ebuf[--elen] = 0; }
    else if (k >= 32 && k < 127 && elen < SET_VAL_MAX - 1) { ebuf[elen++] = (char)k; ebuf[elen] = 0; }
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    int n = (int)settings_count();
    if (e->type == GUI_EV_KEY) {
        int k = e->key;
        if (editing) { edit_key(k); draw(w); return; }
        if (k == GUI_KEY_UP)         { if (sel > 0) sel--; msg[0] = 0; }
        else if (k == GUI_KEY_DOWN)  { if (sel + 1 < n) sel++; msg[0] = 0; }
        else if (k == GUI_KEY_LEFT)  step(-1, 0);
        else if (k == GUI_KEY_RIGHT) step(1, 0);
        else if (k == '+' || k == '=') step(1, 1);
        else if (k == '-')           step(-1, 1);
        else if (k == '\n' || k == ' ') activate();
        else if (k == 'd' || k == 'D') {
            struct setting_info s;
            if (!settings_info((unsigned)sel, &s)) { settings_reset(s.key); say(GUI_C_GOOD, s.key, " reset to default"); }
        }
        else if (k == 'R') { settings_reset(0); say(GUI_C_GOOD, "all settings reset", ""); }
        else return;
        scroll_to_sel();
    } else if (e->type == GUI_EV_MOUSE) {
        if (e->mtype == GUI_MOUSE_WHEEL) {
            topi -= (int)e->arg;
            if (topi > n - VIS) topi = n - VIS;
            if (topi < 0) topi = 0;
        } else if (e->mtype == GUI_MOUSE_DOWN && e->key == 1 && e->my >= LIST_Y && e->my < LIST_Y + VIS * ROW_H) {
            int i = topi + (e->my - LIST_Y) / ROW_H;
            if (i < n) {
                editing = 0;
                if (i == sel) activate(); else { sel = i; msg[0] = 0; }
            }
        } else return;
    } else return;
    draw(w);
}

static void start(struct gui_win *w)
{
    sel = topi = editing = elen = 0;
    ebuf[0] = msg[0] = 0;
    draw(w);
}

const struct gui_app app_settings = {
    .name = "Settings", .glyph = "St", .color = 0x8A5CF6, .w = WIN_W, .h = WIN_H, .tick_ms = 0,
    .start = start, .event = ev,
};

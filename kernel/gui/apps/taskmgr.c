/* Приложение 13: Task Manager — список открытых окон, загрузка процессора, FPS, память поверхностей.
 * Up/Down — выбор, Delete или E — закрыть выбранное окно (End task). */
#include <kernel/gui.h>
#include <miga/string.h>

#define WIN_W 520
#define WIN_H 380
#define ROW_H 30
#define LIST_Y 112

static int sel;
static uint8_t hist[60];                         /* загрузка CPU по секундам */
static unsigned hn;

static void draw(struct gui_win *w)
{
    gui_clear(w, GUI_C_WINBG);
    unsigned cpu = gui_cpu_busy();
    gui_fill(w, 12, 12, WIN_W - 24, 88, GUI_C_PANEL);
    gui_printf(w, 24, 22, GUI_C_TEXT, "CPU busy %u%%", cpu);
    gui_printf(w, 200, 22, GUI_C_TEXT, "Compositor %u / %u fps", gui_fps(), gui_target_hz());
    gui_bar(w, 24, 44, 160, 10, cpu, 100, cpu > 85 ? GUI_C_BAD : GUI_C_ACCENT, GUI_C_PANEL2);
    for (int i = 0; i < 60; i++) {                /* график за минуту */
        unsigned v = hist[(hn + i) % 60];
        int h = (int)(v * 30 / 100);
        gui_fill(w, 24 + i * 8, 90 - h, 6, h ? h : 1, GUI_C_ACCENT);
    }

    int n = gui_task_count();
    if (sel >= n) sel = n ? n - 1 : 0;
    gui_text(w, 24, LIST_Y - 2, "Name", GUI_C_DIM);
    gui_text(w, 250, LIST_Y - 2, "State", GUI_C_DIM);
    gui_text(w, 340, LIST_Y - 2, "Surface", GUI_C_DIM);
    gui_text(w, 430, LIST_Y - 2, "Uptime", GUI_C_DIM);
    int y = LIST_Y + 18;
    for (int i = 0; i < n && y + ROW_H <= WIN_H - 36; i++, y += ROW_H) {
        struct gui_task t;
        if (gui_task_info(i, &t)) break;
        if (i == sel) gui_rrect(w, 12, y, WIN_W - 24, ROW_H - 2, 8, GUI_C_PANEL2);
        gui_rrect(w, 22, y + 5, 18, 18, 5, t.color);
        gui_text(w, 50, y + 6, t.name, GUI_C_TEXT);
        gui_text(w, 250, y + 6, t.minimized ? "minimized" : t.focused ? "active" : "running",
                 t.minimized ? GUI_C_DIM : GUI_C_GOOD);
        gui_printf(w, 340, y + 6, GUI_C_TEXT, "%u KiB", t.surf_kib);
        gui_printf(w, 430, y + 6, GUI_C_TEXT, "%u:%02u", t.up_s / 60, t.up_s % 60);
    }
    gui_printf(w, 12, WIN_H - 26, GUI_C_DIM, "%d window%s    Up/Down select    Del: end task", n, n == 1 ? "" : "s");
    gui_present(w);
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type == GUI_EV_TICK) {
        hist[hn] = (uint8_t)gui_cpu_busy();
        hn = (hn + 1) % 60;
        draw(w);
    } else if (e->type == GUI_EV_KEY) {
        int n = gui_task_count();
        if (e->key == GUI_KEY_UP && sel > 0) sel--;
        else if (e->key == GUI_KEY_DOWN && sel + 1 < n) sel++;
        else if (e->key == GUI_KEY_DEL || e->key == 'e' || e->key == 'E') {
            struct gui_task t;
            if (!gui_task_info(sel, &t) && strcmp(t.name, "Task Manager")) gui_task_end(sel);
            else gui_notify(w, "Cannot end itself");
        }
        draw(w);
    } else if (e->type == GUI_EV_MOUSE && e->mtype == GUI_MOUSE_DOWN) {
        int i = (e->my - (LIST_Y + 18)) / ROW_H;
        if (e->my >= LIST_Y + 18 && i < gui_task_count()) { sel = i; draw(w); }
    }
}
static void start(struct gui_win *w) { sel = 0; memset(hist, 0, sizeof hist); hn = 0; draw(w); }

const struct gui_app app_taskmgr = {
    .name = "Task Manager", .glyph = "TM", .color = 0x14B8A6, .w = WIN_W, .h = WIN_H, .tick_ms = 1000,
    .start = start, .event = ev,
};

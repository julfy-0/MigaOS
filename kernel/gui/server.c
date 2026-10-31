/* MigaGUI server: оконный сервер и композитор.
 *
 * Что делает сервер:
 *   1. владеет фреймбуфером и backbuffer'ом, собирает кадр: обои → виджет и иконки → окна (по z-порядку) →
 *      панель задач → всплывающие слои (календарь, «Пуск», контекстное меню, уведомление);
 *   2. читает клавиатуру (глобальные хоткеи, остальное — окну в фокусе событием GUI_EV_KEY) и мышь (курсор, фокус,
 *      перетаскивание, сворачивание/закрытие окон, панель задач, «Пуск», двойной клик по иконке, GUI_EV_MOUSE клиентам);
 *   3. владеет поверхностями окон (клиенты рисуют в них напрямую) и очередями событий окон;
 *   4. принимает запросы клиентов (damage/title/tick/close/notify) из общей очереди;
 *   5. гоняет таймеры окон (GUI_EV_TICK).
 * Клиенты не знают ни про фреймбуфер, ни про друг друга. */
#define MIGA_TAG "gui"
#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <kernel/gui.h>
#include <kernel/display.h>
#include <kernel/settings.h>
#include "gui_priv.h"
#include <miga/input.h>
#include <miga/time.h>
#include <miga/string.h>
#include <miga/io.h>

/* ------------------------------------------------------------------ приложения */
extern const struct gui_app app_terminal, app_sysmon, app_notepad, app_calc, app_devices, app_snake,
                            app_paint, app_mines, app_dispcheck, app_files, app_settings, app_clock, app_taskmgr;
static const struct gui_app *const apps[] = {&app_terminal, &app_sysmon, &app_notepad, &app_calc, &app_devices,
                                             &app_snake, &app_paint, &app_mines, &app_dispcheck, &app_files,
                                             &app_settings, &app_clock, &app_taskmgr};
#define NAPPS ((int)(sizeof apps / sizeof apps[0]))

/* ------------------------------------------------------------------ геометрия и буферы */
#define MAX_W   4096
#define MAX_H   2304
#define TITLE_H 32
#define TASK_H  56                            /* зона панели задач (внутри — плавающая «таблетка» PILL_H) */
#define PILL_H  44
#define BTN_W   46                            /* ширина кнопок сворачивания/закрытия в заголовке */
#define WIN_R   12                            /* радиус скругления окон */

static struct boot_fb fbi;
static int ready, SW, SH;                     /* размер GUI-области (<= фреймбуфера) */
/* Буферы выделяются в gui_init() из PMM, а не лежат в .bss: 4K-экран = 33 МиБ, статический массив раздул бы
 * образ ядра и перекрыл бы память прошивки (так ядро переставало загружаться). */
static uint32_t *back;                        /* backbuffer, 0x00RRGGBB, stride = SW */
static uint32_t *wall;                        /* готовые обои того же размера (или NULL: тогда простой градиент) */
static uint32_t *surf_mem;                    /* GUI_MAX_WIN поверхностей по GUI_SURF_PX пикселей */
#define SURF_OF(i) (surf_mem + (size_t)(i) * GUI_SURF_PX)

static unsigned target_hz;                    /* частота кадров композитора */
static unsigned fps_now, fps_count, busy_pct; /* кадры композитора за секунду; загрузка процессора, % */

static struct gfx_surf bs;                    /* поверхность backbuffer'а */

/* ------------------------------------------------------------------ тема */
static const uint32_t ACC[8] = {0x7C5CFF, 0x4C8DFF, 0x14B8A6, 0x4CD08A, 0xF2B84B, 0xFF8A4C, 0xF472B6, 0xE5566D};
static const struct { uint32_t top, bot, g2, g3; } WP[5] = {
    {0x0B1030, 0x1A1245, 0x14B8A6, 0xF472B6},   /* 0 Aurora   */
    {0x1E1040, 0xB4456B, 0xFFB86B, 0x6C5CE7},   /* 1 Sunset   */
    {0x03151A, 0x0B4F4A, 0x34D399, 0x3B82F6},   /* 2 Emerald  */
    {0x0E0F14, 0x23262F, 0x64748B, 0x8B5CF6},   /* 3 Graphite */
    {0x02142E, 0x0B5FA5, 0x22D3EE, 0x6366F1},   /* 4 Ocean    */
};
static const char *const WP_NAME[5] = {"Aurora", "Sunset", "Emerald", "Graphite", "Ocean"};
static uint32_t acc = 0x4C8DFF;               /* акцентный цвет (gui.accent) */
static int show_stats = 1;

#define C_SHEET 0x10152A                      /* фон всплывающих панелей */
#define C_EDGE  0x2A3350                      /* тонкая рамка */

/* ------------------------------------------------------------------ окна */
struct window {
    int used, min;
    const struct gui_app *app;
    int x, y, fw, fh;                         /* рамка окна на экране */
    struct gui_win cw;
    char title[24];
    struct gui_event q[GUI_EVQ];
    unsigned qh, qt;
    uint32_t tick_ms, next_tick, born_ms;
};
static struct window wins[GUI_MAX_WIN];
static int zorder[GUI_MAX_WIN], nz;           /* zorder[nz-1] — окно в фокусе; свёрнутые лежат в начале */
static int cascade;

/* ------------------------------------------------------------------ состояние сервера */
enum { MODE_NORMAL, MODE_MOVE };
static int mode, quit;
static int start_open, start_sel, start_n, start_list[32], start_ql;      /* меню «Пуск» */
static char start_q[32];
static int ctx_open, ctx_x, ctx_y, ctx_sel;                               /* контекстное меню рабочего стола */
static int cal_open, cal_y, cal_m;                                        /* календарь над часами */
static char toast_msg[64];
static uint32_t toast_until;
static struct gui_time tm;                                                /* время для панели (раз в секунду) */
static int icon_sel = -1;                     /* выбранная одним кликом иконка рабочего стола */
static uint32_t icon_click_ms;                /* время предыдущего клика по иконке (для двойного клика) */
static int icon_click_idx = -1;
static int dirty, dx0, dy0, dx1, dy1;         /* ограничивающий прямоугольник для вывода на экран */

static struct gui_req rq[GUI_REQQ];
static unsigned rqh, rqt;

static const char *const MONTHS[12] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                       "September", "October", "November", "December"};
static const char *const WDAYS[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

/* ------------------------------------------------------------------ dirty */
static void mark(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SW) x1 = SW;
    if (y1 > SH) y1 = SH;
    if (x >= x1 || y >= y1) return;
    if (!dirty) { dx0 = x; dy0 = y; dx1 = x1; dy1 = y1; dirty = 1; return; }
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}
static void mark_all(void) { mark(0, 0, SW, SH); }
static void mark_win(const struct window *w) { mark(w->x - 18, w->y - 14, w->fw + 36, w->fh + 40); }   /* вместе с тенью */
static void mark_task(void) { mark(0, SH - TASK_H - 44, SW, TASK_H + 44); }                           /* вместе с подсказкой */

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ------------------------------------------------------------------ уведомления */
static void toast(const char *msg)
{
    gui_snprintf(toast_msg, sizeof toast_msg, "%s", msg);
    toast_until = (uint32_t)time_uptime_ms() + 3200;
    mark(0, 0, SW, 90);
}

/* ------------------------------------------------------------------ события окон */
static void post_ev(struct window *w, int type, int key, uint32_t arg)
{
    unsigned n = (w->qh + 1) % GUI_EVQ;
    if (n == w->qt) return;                   /* очередь окна полна: событие теряется */
    w->q[w->qh] = (struct gui_event){.type = (uint8_t)type, .key = (uint8_t)key, .arg = arg};
    w->qh = n;
}

static int dispatch_events(void)
{
    int n = 0;
    for (int i = 0; i < GUI_MAX_WIN; i++) {
        struct window *w = &wins[i];
        while (w->used && w->qt != w->qh) {
            struct gui_event ev = w->q[w->qt];
            w->qt = (w->qt + 1) % GUI_EVQ;
            w->app->event(&w->cw, &ev);
            n++;
        }
    }
    return n;
}

static void run_timers(uint32_t now)
{
    for (int i = 0; i < GUI_MAX_WIN; i++) {
        struct window *w = &wins[i];
        if (!w->used || !w->tick_ms || now < w->next_tick) continue;
        post_ev(w, GUI_EV_TICK, 0, now);
        w->next_tick = now + w->tick_ms;
    }
}

/* ------------------------------------------------------------------ управление окнами */
static int top_win(void) { return nz && !wins[zorder[nz - 1]].min ? zorder[nz - 1] : -1; }

static void zorder_remove(int id)
{
    int j = 0;
    for (int i = 0; i < nz; i++) if (zorder[i] != id) zorder[j++] = zorder[i];
    nz = j;
}

static void focus(int id)
{
    int old = top_win();
    if (old == id) return;
    if (old >= 0) post_ev(&wins[old], GUI_EV_BLUR, 0, 0);
    zorder_remove(id);
    zorder[nz++] = id;
    post_ev(&wins[id], GUI_EV_FOCUS, 0, 0);
    mark_all();
}

static void minimize(int id)
{
    struct window *w = &wins[id];
    if (!w->used || w->min) return;
    int was_top = id == top_win();
    w->min = 1;
    zorder_remove(id);
    for (int i = nz; i > 0; i--) zorder[i] = zorder[i - 1];             /* свёрнутые — в начало списка */
    zorder[0] = id;
    nz++;
    if (was_top) {
        post_ev(w, GUI_EV_BLUR, 0, 0);
        int t = top_win();
        if (t >= 0) post_ev(&wins[t], GUI_EV_FOCUS, 0, 0);
    }
    mark_all();
}

static void restore(int id)
{
    wins[id].min = 0;
    int old = nz >= 2 ? zorder[nz - 1] : -1;
    if (old >= 0 && old != id && !wins[old].min) post_ev(&wins[old], GUI_EV_BLUR, 0, 0);
    zorder_remove(id);
    zorder[nz++] = id;
    post_ev(&wins[id], GUI_EV_FOCUS, 0, 0);
    mark_all();
}

static void clamp_pos(struct window *w)
{
    if (w->x > SW - 60) w->x = SW - 60;
    if (w->x < 60 - w->fw) w->x = 60 - w->fw;
    if (w->y > SH - TASK_H - TITLE_H) w->y = SH - TASK_H - TITLE_H;
    if (w->y < 0) w->y = 0;
}

static int spawn(int idx)
{
    const struct gui_app *a = apps[idx];
    for (int i = 0; i < GUI_MAX_WIN; i++)
        if (wins[i].used && wins[i].app == a) {                          /* одно окно на приложение */
            if (wins[i].min) restore(i); else focus(i);
            return i;
        }

    int s = -1;
    for (int i = 0; i < GUI_MAX_WIN; i++) if (!wins[i].used) { s = i; break; }
    if (s < 0) { toast("Too many windows open"); return -1; }
    if (a->w * a->h > GUI_SURF_PX) return -1;

    struct window *w = &wins[s];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->app = a;
    w->cw.id = s; w->cw.px = SURF_OF(s); w->cw.w = a->w; w->cw.h = a->h;
    for (int i = 0; i < a->w * a->h; i++) w->cw.px[i] = GUI_C_WINBG;
    for (int i = 0; i < (int)sizeof w->title - 1 && a->name[i]; i++) w->title[i] = a->name[i];
    w->fw = a->w + 2;
    w->fh = a->h + TITLE_H + 1;
    w->x = 150 + (cascade % 6) * 30;
    w->y = 30 + (cascade % 6) * 26;
    cascade++;
    if (w->x + w->fw > SW) w->x = SW - w->fw;
    if (w->y + w->fh > SH - TASK_H) w->y = SH - TASK_H - w->fh;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    w->tick_ms = a->tick_ms;
    w->born_ms = (uint32_t)time_uptime_ms();
    w->next_tick = w->born_ms + a->tick_ms;

    int old = top_win();
    if (old >= 0) post_ev(&wins[old], GUI_EV_BLUR, 0, 0);
    zorder[nz++] = s;
    a->start(&w->cw);
    post_ev(w, GUI_EV_FOCUS, 0, 0);
    mark_all();
    return s;
}

static void close_win(int id)
{
    struct window *w = &wins[id];
    if (!w->used) return;
    w->app->event(&w->cw, &(struct gui_event){.type = GUI_EV_CLOSE});
    w->used = 0;
    zorder_remove(id);
    int t = top_win();
    if (t >= 0) post_ev(&wins[t], GUI_EV_FOCUS, 0, 0);
    mark_all();
}

/* ------------------------------------------------------------------ очередь запросов клиентов */
static void handle_req(const struct gui_req *r)
{
    if (r->win >= GUI_MAX_WIN || !wins[r->win].used) return;
    struct window *w = &wins[r->win];
    switch (r->type) {
    case GUI_RQ_DAMAGE:
        if (w->min) break;
        if (r->w <= 0 || r->h <= 0) mark(w->x + 1, w->y + TITLE_H, w->cw.w, w->cw.h);
        else mark(w->x + 1 + r->x, w->y + TITLE_H + r->y, r->w, r->h);
        break;
    case GUI_RQ_TITLE:
        memset(w->title, 0, sizeof w->title);
        for (unsigned i = 0; i < sizeof w->title - 1 && r->s[i]; i++) w->title[i] = r->s[i];
        mark(w->x, w->y, w->fw, TITLE_H);
        break;
    case GUI_RQ_TICK:
        w->tick_ms = r->x > 0 ? (uint32_t)r->x : 0;
        w->next_tick = (uint32_t)time_uptime_ms() + w->tick_ms;
        break;
    case GUI_RQ_CLOSE:
        close_win(r->win);
        break;
    case GUI_RQ_NOTIFY: {
        char m[sizeof r->s + 1];
        memcpy(m, r->s, sizeof r->s);
        m[sizeof r->s] = 0;
        toast(m);
        break; }
    }
}

static int drain_requests(void)
{
    int n = 0;
    while (rqt != rqh) {
        struct gui_req r = rq[rqt];
        rqt = (rqt + 1) % GUI_REQQ;
        handle_req(&r);
        n++;
    }
    return n;
}

int gui_srv_post(const struct gui_req *r)
{
    unsigned n = (rqh + 1) % GUI_REQQ;
    if (n == rqt) { drain_requests(); n = (rqh + 1) % GUI_REQQ; }   /* очередь полна: сервер обрабатывает сам */
    rq[rqh] = *r;
    rqh = n;
    return 0;
}

/* ------------------------------------------------------------------ геометрия элементов (общая для рисования и мыши) */
static void icon_pos(int i, int *x, int *y)                 /* левый верхний угол ячейки иконки рабочего стола */
{
    int per = (SH - TASK_H - 16) / 92;
    if (per < 1) per = 1;
    *x = 14 + (i / per) * 122; *y = 14 + (i % per) * 92;
}
#define ICON_CELL_W 118
#define ICON_CELL_H 88

/* «Пуск» */
#define SM_W    560
#define SM_COLS 4
static int sm_tile_h, sm_slots;
static void start_geom(int *x, int *y, int *w, int *h)
{
    int avail = SH - TASK_H - 16;
    sm_tile_h = 92;
    int rows = (NAPPS + SM_COLS - 1) / SM_COLS;
    if (20 + 44 + 16 + 20 + rows * sm_tile_h + 12 + 60 > avail) {
        sm_tile_h = 72;
        int fit = (avail - 172) / sm_tile_h;
        if (fit < rows) rows = fit < 1 ? 1 : fit;
    }
    sm_slots = rows * SM_COLS;
    *w = SM_W; *h = 20 + 44 + 16 + 20 + rows * sm_tile_h + 12 + 60;
    *x = (SW - *w) / 2; *y = SH - TASK_H - 8 - *h;
}
static void tile_rect(int slot, int *x, int *y, int *w, int *h)
{
    int sx, sy, sw, sh;
    start_geom(&sx, &sy, &sw, &sh);
    int tw = (SM_W - 40) / SM_COLS;
    *x = sx + 20 + (slot % SM_COLS) * tw; *y = sy + 100 + (slot / SM_COLS) * sm_tile_h; *w = tw; *h = sm_tile_h;
}
static void power_btn_rect(int i, int *x, int *y, int *w, int *h)        /* 0 Exit GUI, 1 Restart, 2 Power off */
{
    static const int bw[3] = {92, 84, 100};
    int sx, sy, sw, sh;
    start_geom(&sx, &sy, &sw, &sh);
    int cx = sx + sw - 20;
    for (int k = 2; k >= i; k--) cx -= bw[k] + (k == 2 ? 0 : 8);
    *x = cx; *y = sy + sh - 48; *w = bw[i]; *h = 34;
}

/* контекстное меню */
static const struct { const char *label; int act; int sep; } CTX[] = {
    {"Open Terminal", 1, 0}, {"Files", 2, 0}, {"Task Manager", 3, 0}, {"Settings", 4, 0},
    {"Next wallpaper", 5, 1}, {"Next accent color", 6, 0},
    {"About MigaOS", 7, 1}, {"Exit to text console", 8, 0},
};
#define NCTX ((int)(sizeof CTX / sizeof CTX[0]))
#define CTX_W 230
static int ctx_h(void) { int h = 12; for (int i = 0; i < NCTX; i++) h += 32 + (CTX[i].sep ? 9 : 0); return h; }
static int ctx_item_y(int i) { int y = ctx_y + 6; for (int k = 0; k < i; k++) y += 32 + (CTX[k].sep ? 9 : 0); return y + (CTX[i].sep ? 9 : 0); }

/* календарь */
#define CAL_W 330
#define CAL_H 380
static void cal_geom(int *x, int *y) { *x = SW - CAL_W - 10; *y = SH - TASK_H - 8 - CAL_H; }

/* панель задач */
#define TASK_NONE  (-3)
#define TASK_START (-1)
#define TASK_TRAY  (-2)
struct tl { int px, pw, py, n, tx, tw; };
static void task_layout(struct tl *t)
{
    int n = 0;
    for (int i = 0; i < GUI_MAX_WIN; i++) n += wins[i].used;
    t->n = n;
    t->pw = 8 + 40 + (n ? 8 + n * 44 : 0) + 8;
    t->px = (SW - t->pw) / 2;
    t->py = SH - TASK_H + (TASK_H - PILL_H) / 2;
    char hz[40];
    gui_snprintf(hz, sizeof hz, "%uHz %ufps", display_refresh_hz(), fps_now);
    t->tw = 20 + 76 + (show_stats ? gui_text_width(hz) + 14 : 0);
    t->tx = SW - t->tw - 10;
}
static int task_btn_x(const struct tl *t, int k) { return t->px + 8 + 40 + 8 + k * 44; }

static int task_hit(int x, int y)
{
    struct tl t;
    task_layout(&t);
    if (y < t.py || y >= t.py + PILL_H) return TASK_NONE;
    if (x >= t.tx && x < t.tx + t.tw) return TASK_TRAY;
    if (x >= t.px + 8 && x < t.px + 48) return TASK_START;
    int k = 0;
    for (int i = 0; i < GUI_MAX_WIN; i++) {
        if (!wins[i].used) continue;
        int bx = task_btn_x(&t, k++);
        if (x >= bx && x < bx + 44) return i;
    }
    return TASK_NONE;
}

static int close_btn_hit(const struct window *w, int x, int y) { return in_rect(x, y, w->x + w->fw - 1 - BTN_W, w->y + 1, BTN_W, TITLE_H - 1); }
static int min_btn_hit(const struct window *w, int x, int y) { return in_rect(x, y, w->x + w->fw - 1 - 2 * BTN_W, w->y + 1, BTN_W, TITLE_H - 1); }

/* Верхнее окно под точкой или -1 (свёрнутые не в счёт). */
static int win_at(int x, int y)
{
    for (int k = nz - 1; k >= 0; k--) {
        struct window *w = &wins[zorder[k]];
        if (!w->min && in_rect(x, y, w->x, w->y, w->fw, w->fh)) return zorder[k];
    }
    return -1;
}

/* ------------------------------------------------------------------ «Пуск»: поиск */
static int ci_contains(const char *hay, const char *needle)
{
    if (!*needle) return 1;
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static void start_refilter(void)
{
    start_n = 0;
    for (int i = 0; i < NAPPS && start_n < 32; i++) if (ci_contains(apps[i]->name, start_q)) start_list[start_n++] = i;
    if (start_sel >= start_n) start_sel = start_n ? start_n - 1 : 0;
}

static void popups_close(void)
{
    if (start_open || ctx_open || cal_open) mark_all();
    start_open = ctx_open = cal_open = 0;
}

static void start_toggle(void)
{
    int was = start_open;
    popups_close();
    if (!was) { start_open = 1; start_q[0] = 0; start_ql = 0; start_sel = 0; start_refilter(); }
    mark_all();
}

static void start_launch(int app) { popups_close(); if (app >= 0 && app < NAPPS) spawn(app); }

/* ------------------------------------------------------------------ питание */
static void power_action(int kind);
static void draw_frame_now(void);

static void power_action(int kind)
{
    if (kind == 0) { popups_close(); quit = 1; return; }
    popups_close();
    toast(kind == 1 ? "Restarting..." : "Shutting down...");
    draw_frame_now();
    if (kind == 1) { for (int i = 0; i < 3; i++) { outb(0x64, 0xFE); for (volatile int k = 0; k < 2000000; k++) {} } }
    else { outw(0x604, 0x2000); outw(0xB004, 0x2000); outw(0x4004, 0x3400); }      /* ACPI PM1a: QEMU q35, Bochs, VirtualBox */
    toast("It is now safe to turn off the computer");
    draw_frame_now();
    cpu_cli();
    for (;;) cpu_hlt();
}

/* ------------------------------------------------------------------ тема и обои */
static void build_wall(void)
{
    int st = settings_get_int("gui.wallpaper", 0);
    if (st < 0 || st > 4) st = 0;
    if (!wall) return;
    struct gfx_surf ws = {wall, SW, SH};
    for (int y = 0; y < SH; y++) {
        int t = y * 255 / (SH > 1 ? SH - 1 : 1);
        uint32_t a = WP[st].top, b = WP[st].bot, c = 0;
        for (int sh = 0; sh < 24; sh += 8) c |= ((((a >> sh) & 255) * (255 - t) + ((b >> sh) & 255) * t) / 255) << sh;
        gfx_fill(&ws, 0, y, SW, 1, c);
    }
    gfx_glow(&ws, SW * 18 / 100, SH * 28 / 100, SW * 45 / 100, acc, 150);
    gfx_glow(&ws, SW * 82 / 100, SH * 18 / 100, SW * 38 / 100, WP[st].g2, 120);
    gfx_glow(&ws, SW * 60 / 100, SH, SW * 50 / 100, WP[st].g3, 130);
}

static void load_theme(void)
{
    int a = settings_get_int("gui.accent", 1);
    acc = ACC[a < 0 ? 0 : a > 7 ? 7 : a];
    show_stats = settings_get_int("gui.stats", 1);
    build_wall();
}

void gui_theme_changed(void)
{
    if (!ready) return;
    load_theme();
    gui_get_time(&tm);
    mark_all();
}

static void cycle_setting(const char *key, int max, const char *what, const char *const *names)
{
    int v = (settings_get_int(key, 0) + 1) % (max + 1);
    settings_set_int(key, v);                         /* apply-колбэк вызовет gui_theme_changed() */
    char m[48];
    gui_snprintf(m, sizeof m, "%s: %s", what, names ? names[v] : "changed");
    toast(m);
}

static const char *const ACC_NAME[8] = {"Violet", "Blue", "Teal", "Green", "Amber", "Orange", "Pink", "Red"};

/* ------------------------------------------------------------------ ввод: клавиатура */
static void move_window(int dxp, int dyp)
{
    int t = top_win();
    if (t < 0) return;
    struct window *w = &wins[t];
    mark_win(w);
    w->x += dxp; w->y += dyp;
    clamp_pos(w);
    mark_win(w);
}

static void ctx_do(int act)
{
    popups_close();
    switch (act) {
    case 1: spawn(0); break;
    case 2: gui_launch("Files"); break;
    case 3: gui_launch("Task Manager"); break;
    case 4: gui_launch("Settings"); break;
    case 5: cycle_setting("gui.wallpaper", 4, "Wallpaper", WP_NAME); break;
    case 6: cycle_setting("gui.accent", 7, "Accent", ACC_NAME); break;
    case 7: toast("MigaOS 0.1  -  MigaGUI compositor"); break;
    case 8: quit = 1; break;
    }
}

static void start_key(int c)
{
    if (c == GUI_KEY_SUPER) { start_toggle(); return; }
    if (c == GUI_KEY_ESC) {
        if (start_ql) { start_ql = 0; start_q[0] = 0; start_sel = 0; start_refilter(); }
        else start_toggle();
    } else if (c == GUI_KEY_LEFT)  { if (start_sel > 0) start_sel--; }
    else if (c == GUI_KEY_RIGHT) { if (start_sel + 1 < start_n) start_sel++; }
    else if (c == GUI_KEY_UP)    { if (start_sel >= SM_COLS) start_sel -= SM_COLS; }
    else if (c == GUI_KEY_DOWN)  { if (start_sel + SM_COLS < start_n) start_sel += SM_COLS; else if (start_n) start_sel = start_n - 1; }
    else if (c == '\n') { if (start_n) start_launch(start_list[start_sel]); return; }
    else if (c == '\b') { if (start_ql) { start_q[--start_ql] = 0; start_sel = 0; start_refilter(); } }
    else if (c >= 32 && c < 127 && start_ql < (int)sizeof start_q - 1) { start_q[start_ql++] = (char)c; start_q[start_ql] = 0; start_sel = 0; start_refilter(); }
    int sx, sy, sw, sh;
    start_geom(&sx, &sy, &sw, &sh);
    mark(sx - 20, sy - 20, sw + 40, sh + 40);
}

static void handle_key(int c)
{
    if (c == GUI_KEY_SUPER) { start_toggle(); return; }                       /* Win / Super */
    if (mode == MODE_MOVE) {
        switch (c) {
        case GUI_KEY_UP:    move_window(0, -16); break;
        case GUI_KEY_DOWN:  move_window(0, 16);  break;
        case GUI_KEY_LEFT:  move_window(-16, 0); break;
        case GUI_KEY_RIGHT: move_window(16, 0);  break;
        case '\n': case GUI_KEY_ESC: mode = MODE_NORMAL; mark_all(); break;
        }
        return;
    }
    if (start_open) { start_key(c); return; }
    if (ctx_open) {
        if (c == GUI_KEY_ESC) popups_close();
        else if (c == GUI_KEY_UP)   { ctx_sel = (ctx_sel + NCTX - 1) % NCTX; mark(ctx_x - 20, ctx_y - 20, CTX_W + 40, ctx_h() + 40); }
        else if (c == GUI_KEY_DOWN) { ctx_sel = (ctx_sel + 1) % NCTX; mark(ctx_x - 20, ctx_y - 20, CTX_W + 40, ctx_h() + 40); }
        else if (c == '\n') ctx_do(CTX[ctx_sel].act);
        return;
    }
    if (cal_open) {
        int cx, cy;
        cal_geom(&cx, &cy);
        if (c == GUI_KEY_ESC) popups_close();
        else if (c == GUI_KEY_LEFT)  { if (--cal_m < 1) { cal_m = 12; cal_y--; } mark(cx, cy, CAL_W, CAL_H); }
        else if (c == GUI_KEY_RIGHT) { if (++cal_m > 12) { cal_m = 1; cal_y++; } mark(cx, cy, CAL_W, CAL_H); }
        return;
    }
    switch (c) {
    case GUI_KEY_ESC: start_toggle(); return;
    case 0x0E:                                   /* Ctrl+N: следующее окно */
        for (int i = 0; i < nz; i++) if (!wins[zorder[i]].min && zorder[i] != top_win()) { focus(zorder[i]); break; }
        return;
    case 0x0D: { int t = top_win(); if (t >= 0) minimize(t); return; }      /* Ctrl+M: свернуть */
    case 0x04: {                                                             /* Ctrl+D: свернуть все / вернуть */
        int any = 0;
        for (int i = 0; i < GUI_MAX_WIN; i++) if (wins[i].used && !wins[i].min) any = 1;
        for (int i = 0; i < GUI_MAX_WIN; i++) if (wins[i].used) { if (any) minimize(i); else if (wins[i].min) restore(i); }
        return; }
    case 0x17: if (top_win() >= 0) close_win(top_win()); return;            /* Ctrl+W */
    case 0x07: if (top_win() >= 0) { mode = MODE_MOVE; mark_all(); } return;/* Ctrl+G: переместить окно */
    case 0x11: quit = 1; return;                 /* Ctrl+Q */
    }
    if (top_win() < 0) {                         /* на пустом рабочем столе цифры запускают приложения */
        if (c >= '1' && c <= '9' && c < '1' + NAPPS) spawn(c - '1');
        return;
    }
    post_ev(&wins[top_win()], GUI_EV_KEY, c, 0);
}

/* ------------------------------------------------------------------ курсор мыши */
static int cur_x, cur_y;                      /* позиция горячей точки */
static int cur_drawn, cur_dx, cur_dy;         /* нарисован ли курсор во фреймбуфере и где */
static int cur_scale = 1;
static unsigned btn_state;
static int hover_close = -1, hover_min = -1, hover_task = TASK_NONE, hover_tile = -1, hover_pbtn = -1, hover_ctx = -1, hover_cal = 0;
static int drag_win = -1, drag_ox, drag_oy;   /* перетаскиваемое окно и смещение курсора внутри рамки */
static int capture_win = -1;                  /* окно, получившее нажатие: ему уйдёт и отпускание кнопки */

static uint32_t swap_rb(uint32_t c) { return (c & 0x00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16); }

static inline void fb_put(int x, int y, uint32_t rgb)
{
    uint32_t *d = (uint32_t *)((uint8_t *)fbi.base + (uint64_t)y * fbi.pitch) + x;
    *d = fbi.format == FB_BGRX ? rgb : swap_rb(rgb);
}

/* Курсор = сам фреймбуфер минус backbuffer: убрать курсор значит вернуть под ним пиксели из back, нарисовать — записать
 * пиксели спрайта. Кадр при этом не пересобирается, поэтому мышь реагирует мгновенно даже на 4K/240 Гц. */
static void cursor_erase(void)
{
    if (!cur_drawn) return;
    const uint8_t *sh = cursor_shape();
    for (int y = 0; y < CUR_H; y++)
        for (int x = 0; x < CUR_W; x++) {
            if (!sh[y * CUR_W + x]) continue;
            for (int j = 0; j < cur_scale; j++)
                for (int i = 0; i < cur_scale; i++) {
                    int px = cur_dx + x * cur_scale + i, py = cur_dy + y * cur_scale + j;
                    if (px < 0 || py < 0 || px >= SW || py >= SH) continue;
                    fb_put(px, py, back[(long)py * SW + px]);
                }
        }
    cur_drawn = 0;
}

static void cursor_paint(void)
{
    const uint8_t *sh = cursor_shape();
    for (int y = 0; y < CUR_H; y++)
        for (int x = 0; x < CUR_W; x++) {
            uint8_t v = sh[y * CUR_W + x];
            if (!v) continue;
            uint32_t col = v == 1 ? 0x000000 : 0xFFFFFF;
            for (int j = 0; j < cur_scale; j++)
                for (int i = 0; i < cur_scale; i++) {
                    int px = cur_x + x * cur_scale + i, py = cur_y + y * cur_scale + j;
                    if (px < 0 || py < 0 || px >= SW || py >= SH) continue;
                    fb_put(px, py, col);
                }
        }
    cur_dx = cur_x; cur_dy = cur_y; cur_drawn = 1;
}

/* ------------------------------------------------------------------ мышь: события */
static void post_mouse(struct window *w, int mtype, int key, int mx, int my, int arg)
{
    if (mtype == GUI_MOUSE_MOVE && w->qh != w->qt) {            /* склеиваем подряд идущие MOVE: очередь не засоряем */
        struct gui_event *l = &w->q[(w->qh + GUI_EVQ - 1) % GUI_EVQ];
        if (l->type == GUI_EV_MOUSE && l->mtype == GUI_MOUSE_MOVE) { l->mx = (int16_t)mx; l->my = (int16_t)my; l->buttons = (uint8_t)btn_state; return; }
    }
    unsigned n = (w->qh + 1) % GUI_EVQ;
    if (n == w->qt) return;
    w->q[w->qh] = (struct gui_event){.type = GUI_EV_MOUSE, .mtype = (uint8_t)mtype, .key = (uint8_t)key,
                                     .buttons = (uint8_t)btn_state, .arg = (uint32_t)arg,
                                     .mx = (int16_t)mx, .my = (int16_t)my};
    w->qh = n;
}

static void update_hover(void)
{
    int hc = -1, hm = -1, ht = TASK_NONE, htile = -1, hp = -1, hx = -1, hcal = 0;
    if (ctx_open) {
        for (int i = 0; i < NCTX; i++) if (in_rect(cur_x, cur_y, ctx_x + 6, ctx_item_y(i), CTX_W - 12, 32)) hx = i;
    } else if (start_open) {
        for (int s = 0; s < sm_slots && s < start_n; s++) {
            int x, y, w, h;
            tile_rect(s, &x, &y, &w, &h);
            if (in_rect(cur_x, cur_y, x, y, w, h)) htile = s;
        }
        for (int i = 0; i < 3; i++) {
            int x, y, w, h;
            power_btn_rect(i, &x, &y, &w, &h);
            if (in_rect(cur_x, cur_y, x, y, w, h)) hp = i;
        }
    } else if (cal_open) {
        int cx, cy;
        cal_geom(&cx, &cy);
        if (in_rect(cur_x, cur_y, cx + 14, cy + 94, 32, 32)) hcal = 1;
        else if (in_rect(cur_x, cur_y, cx + CAL_W - 46, cy + 94, 32, 32)) hcal = 2;
    }
    if (cur_y >= SH - TASK_H) ht = task_hit(cur_x, cur_y);
    else if (!start_open && !ctx_open && !cal_open) {
        int w = win_at(cur_x, cur_y);
        if (w >= 0 && close_btn_hit(&wins[w], cur_x, cur_y)) hc = w;
        if (w >= 0 && min_btn_hit(&wins[w], cur_x, cur_y)) hm = w;
    }
    if (hc != hover_close) {
        if (hover_close >= 0 && wins[hover_close].used) mark(wins[hover_close].x, wins[hover_close].y, wins[hover_close].fw, TITLE_H);
        if (hc >= 0) mark(wins[hc].x, wins[hc].y, wins[hc].fw, TITLE_H);
        hover_close = hc;
    }
    if (hm != hover_min) {
        if (hover_min >= 0 && wins[hover_min].used) mark(wins[hover_min].x, wins[hover_min].y, wins[hover_min].fw, TITLE_H);
        if (hm >= 0) mark(wins[hm].x, wins[hm].y, wins[hm].fw, TITLE_H);
        hover_min = hm;
    }
    if (ht != hover_task) { hover_task = ht; mark_task(); }
    int sx, sy, sw, sh;
    start_geom(&sx, &sy, &sw, &sh);
    if (htile != hover_tile || hp != hover_pbtn) {
        if (htile >= 0 && htile != start_sel) start_sel = htile;
        hover_tile = htile; hover_pbtn = hp;
        mark(sx, sy, sw, sh);
    }
    if (hx != hover_ctx) { if (hx >= 0) ctx_sel = hx; hover_ctx = hx; mark(ctx_x, ctx_y, CTX_W, ctx_h()); }
    if (hcal != hover_cal) { hover_cal = hcal; int cx, cy; cal_geom(&cx, &cy); mark(cx, cy, CAL_W, CAL_H); }
}

static void on_left_down(void)
{
    int x = cur_x, y = cur_y;

    if (ctx_open) {                                             /* контекстное меню */
        for (int i = 0; i < NCTX; i++)
            if (in_rect(x, y, ctx_x + 6, ctx_item_y(i), CTX_W - 12, 32)) { ctx_do(CTX[i].act); return; }
        popups_close();
        return;
    }
    if (y >= SH - TASK_H) {                                     /* панель задач */
        int t = task_hit(x, y);
        if (t == TASK_START) start_toggle();
        else if (t == TASK_TRAY) {
            int was = cal_open;
            popups_close();
            if (!was) { cal_open = 1; cal_y = tm.year; cal_m = tm.mon; }
            mark_all();
        } else if (t >= 0) {
            popups_close();
            if (wins[t].min) restore(t);
            else if (t == top_win()) minimize(t);
            else focus(t);
        } else popups_close();
        return;
    }
    if (start_open) {
        int sx, sy, sw, sh;
        start_geom(&sx, &sy, &sw, &sh);
        for (int s = 0; s < sm_slots && s < start_n; s++) {
            int tx, ty, tw, th;
            tile_rect(s, &tx, &ty, &tw, &th);
            if (in_rect(x, y, tx, ty, tw, th)) { start_launch(start_list[s]); return; }
        }
        for (int i = 0; i < 3; i++) {
            int bx, by, bw, bh;
            power_btn_rect(i, &bx, &by, &bw, &bh);
            if (in_rect(x, y, bx, by, bw, bh)) { power_action(i); return; }
        }
        int inside = in_rect(x, y, sx, sy, sw, sh);
        popups_close();                                         /* клик мимо меню закрывает его */
        if (inside) return;
    }
    if (cal_open) {
        int cx, cy;
        cal_geom(&cx, &cy);
        if (in_rect(x, y, cx + 14, cy + 94, 32, 32)) { if (--cal_m < 1) { cal_m = 12; cal_y--; } mark(cx, cy, CAL_W, CAL_H); return; }
        if (in_rect(x, y, cx + CAL_W - 46, cy + 94, 32, 32)) { if (++cal_m > 12) { cal_m = 1; cal_y++; } mark(cx, cy, CAL_W, CAL_H); return; }
        int inside = in_rect(x, y, cx, cy, CAL_W, CAL_H);
        popups_close();
        if (inside) return;
    }
    int id = win_at(x, y);
    if (id >= 0) {
        struct window *w = &wins[id];
        if (id != top_win()) focus(id);
        if (y < w->y + TITLE_H) {
            if (close_btn_hit(w, x, y)) { close_win(id); update_hover(); return; }
            if (min_btn_hit(w, x, y)) { minimize(id); update_hover(); return; }
            drag_win = id; drag_ox = x - w->x; drag_oy = y - w->y;      /* тянем окно за заголовок */
        } else {
            capture_win = id;
            post_mouse(w, GUI_MOUSE_DOWN, MOUSE_BTN_LEFT, x - w->x - 1, y - w->y - TITLE_H, 0);
        }
        return;
    }
    for (int i = 0; i < NAPPS; i++) {                           /* иконки рабочего стола: двойной клик запускает */
        int ix, iy;
        icon_pos(i, &ix, &iy);
        if (!in_rect(x, y, ix, iy, ICON_CELL_W, ICON_CELL_H)) continue;
        uint32_t now = (uint32_t)time_uptime_ms();
        if (icon_click_idx == i && now - icon_click_ms < 500) { icon_click_idx = -1; icon_sel = -1; spawn(i); }
        else { icon_click_idx = i; icon_click_ms = now; icon_sel = i; }
        mark_all();
        return;
    }
    if (icon_sel >= 0) { icon_sel = -1; mark_all(); }
    icon_click_idx = -1;
}

static void on_button(unsigned bit, int down)
{
    if (mode == MODE_MOVE) return;
    if (bit == MOUSE_BTN_LEFT) {
        if (down) on_left_down();
        else {
            drag_win = -1;
            if (capture_win >= 0 && wins[capture_win].used) {
                struct window *w = &wins[capture_win];
                post_mouse(w, GUI_MOUSE_UP, MOUSE_BTN_LEFT, cur_x - w->x - 1, cur_y - w->y - TITLE_H, 0);
            }
            capture_win = -1;
        }
    } else if (down) {
        if (start_open || cal_open || ctx_open) { popups_close(); return; }
        int id = win_at(cur_x, cur_y);
        if (id >= 0 && cur_y >= wins[id].y + TITLE_H) {
            struct window *w = &wins[id];
            if (id != top_win()) focus(id);
            post_mouse(w, GUI_MOUSE_DOWN, (int)bit, cur_x - w->x - 1, cur_y - w->y - TITLE_H, 0);
        } else if (id < 0 && bit == MOUSE_BTN_RIGHT && cur_y < SH - TASK_H) {
            ctx_open = 1; ctx_sel = -1;                         /* правая кнопка по рабочему столу — контекстное меню */
            ctx_x = imin(cur_x, SW - CTX_W - 8); ctx_y = imin(cur_y, SH - TASK_H - ctx_h() - 8);
            if (ctx_y < 4) ctx_y = 4;
            mark(ctx_x - 20, ctx_y - 20, CTX_W + 40, ctx_h() + 40);
            update_hover();
        }
    }
}

static void on_wheel(int notches)
{
    if (start_open) {
        int d = notches > 0 ? -SM_COLS : SM_COLS;
        if (start_sel + d >= 0 && start_sel + d < start_n) start_sel += d;
        int sx, sy, sw, sh;
        start_geom(&sx, &sy, &sw, &sh);
        mark(sx, sy, sw, sh);
        return;
    }
    int id = win_at(cur_x, cur_y);
    if (id < 0 || ctx_open || cal_open) return;
    struct window *w = &wins[id];
    post_mouse(w, GUI_MOUSE_WHEEL, 0, cur_x - w->x - 1, cur_y - w->y - TITLE_H, notches);
    for (int n = notches < 0 ? -notches : notches; n > 0; n--)    /* приложения без поддержки мыши получают стрелки */
        post_ev(w, GUI_EV_KEY, notches > 0 ? GUI_KEY_UP : GUI_KEY_DOWN, 0);
}

/* ------------------------------------------------------------------ мышь: движение с ускорением */
static int acc_dx, acc_dy, rem_x, rem_y;      /* накопленные отсчёты и дробный остаток (в 1/256 пикселя) */
static uint64_t last_flush_us;

/* Как «повышенная точность указателя» в Windows: чем быстрее рука, тем больше пикселей на отсчёт (1x..2,5x).
 * Скорость считаем в отсчётах за мс, поэтому кривая не зависит от частоты опроса мыши (125...1000 Гц). */
static void flush_motion(uint64_t now_us)
{
    if (!acc_dx && !acc_dy) { last_flush_us = now_us; return; }
    uint64_t dt = now_us - last_flush_us;
    if (dt < 1000) dt = 1000;
    if (dt > 50000) dt = 50000;
    int mag = (acc_dx < 0 ? -acc_dx : acc_dx) + (acc_dy < 0 ? -acc_dy : acc_dy);
    int v = (int)((uint64_t)mag * 100000 / dt);                 /* отсчётов в мс, x100 */
    int g = 256;
    if (v > 800) g = 640;
    else if (v > 80) g = 256 + (v - 80) * 384 / 720;

    int qx = acc_dx * g + rem_x, qy = acc_dy * g + rem_y;
    int ox = qx >> 8, oy = qy >> 8;
    rem_x = qx - (ox << 8); rem_y = qy - (oy << 8);
    acc_dx = acc_dy = 0;
    last_flush_us = now_us;

    int nx = cur_x + ox, ny = cur_y + oy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > SW - 1) nx = SW - 1;
    if (ny > SH - 1) ny = SH - 1;
    if (nx == cur_x && ny == cur_y) return;

    cursor_erase();
    cur_x = nx; cur_y = ny;
    cursor_paint();

    if (drag_win >= 0 && wins[drag_win].used) {                 /* перетаскивание окна */
        struct window *w = &wins[drag_win];
        mark_win(w);
        w->x = cur_x - drag_ox; w->y = cur_y - drag_oy;
        clamp_pos(w);
        mark_win(w);
    } else if (capture_win >= 0 && wins[capture_win].used) {
        struct window *w = &wins[capture_win];
        post_mouse(w, GUI_MOUSE_MOVE, 0, cur_x - w->x - 1, cur_y - w->y - TITLE_H, 0);
    }
    update_hover();
}

static void process_mouse(uint64_t now_us)
{
    struct mouse_pkt p;
    while (mouse_pop(&p) == 0) {
        acc_dx += p.dx; acc_dy += p.dy;
        if (p.wheel) { flush_motion(now_us); on_wheel(p.wheel); }
        unsigned changed = p.buttons ^ btn_state;
        if (changed) {
            flush_motion(now_us);                                /* сначала доехать до точки клика */
            for (unsigned bit = 1; bit <= 4; bit <<= 1)
                if (changed & bit) { btn_state = (btn_state & ~bit) | (p.buttons & bit); on_button(bit, (p.buttons & bit) != 0); }
        }
    }
    if ((acc_dx || acc_dy) && now_us - last_flush_us >= 1000) flush_motion(now_us);
}

/* ------------------------------------------------------------------ рисование кадра */
static void text_c(int x, int y, int w, const char *s, uint32_t c) { gfx_text(&bs, x + (w - gui_text_width(s)) / 2, y, s, c, 0, 0); }
static void text_r(int x, int y, int w, const char *s, uint32_t c) { gfx_text(&bs, x + w - gui_text_width(s), y, s, c, 0, 0); }

static void draw_wallpaper(int x0, int y0, int x1, int y1)
{
    if (wall) {
        for (int y = y0; y < y1; y++) memcpy(back + (long)y * SW + x0, wall + (long)y * SW + x0, (size_t)(x1 - x0) * 4);
        return;
    }
    for (int y = y0; y < y1; y++) {
        uint32_t r = 0x0C + (0x14 * y) / SH, g = 0x14 + (0x28 * y) / SH, b = 0x30 + (0x40 * y) / SH;
        gfx_fill(&bs, 0, y, SW, 1, (r << 16) | (g << 8) | b);
    }
}

/* Иконка приложения: скруглённая плитка цвета приложения с бликом и двумя буквами. */
static void draw_app_icon(int x, int y, int size, const struct gui_app *a, int scale)
{
    int r = size / 4;
    gfx_rrect(&bs, x, y, size, size, r, a->color, 255);
    gfx_rrect_m(&bs, x, y, size, size / 2, r, 3, 0xFFFFFF, 34);
    gfx_fill_a(&bs, x + r, y + 1, size - 2 * r, 1, 0xFFFFFF, 90);
    char g[3] = {a->glyph[0], a->glyph[1], 0};
    int gw = 16 * scale, gh = 16 * scale;
    gfx_text_big(&bs, x + (size - gw) / 2 + 1, y + (size - gh) / 2 + 1, g, 0x0A0D18, scale);       /* тень глифа */
    gfx_text_big(&bs, x + (size - gw) / 2, y + (size - gh) / 2, g, 0xFFFFFF, scale);
}

static void draw_widget(void)                       /* большие часы на рабочем столе */
{
    char t[8], d[40];
    gui_snprintf(t, sizeof t, "%02d:%02d", tm.hour, tm.min);
    gui_snprintf(d, sizeof d, "%s, %d %s", WDAYS[tm.wday], tm.day, MONTHS[tm.mon - 1]);
    int tw = gui_text_width(t) * 4, dw = gui_text_width(d) * 2;
    int x = SW - 28, y = 24;
    if (!gfx_clip_hit(&bs, x - imax(tw, dw) - 6, y, imax(tw, dw) + 12, 110)) return;
    gfx_text_big(&bs, x - tw + 3, y + 3, t, 0x05070C, 4);
    gfx_text_big(&bs, x - tw, y, t, 0xF2F5FF, 4);
    gfx_text_big(&bs, x - dw, y + 76, d, 0xB5C0E2, 2);
}

static void draw_icons(void)
{
    for (int i = 0; i < NAPPS; i++) {
        int x, y;
        icon_pos(i, &x, &y);
        if (!gfx_clip_hit(&bs, x, y, ICON_CELL_W, ICON_CELL_H)) continue;
        if (i == icon_sel) {
            gfx_rrect(&bs, x, y, ICON_CELL_W, ICON_CELL_H, 12, acc, 70);
            gfx_rrect(&bs, x + 1, y + 1, ICON_CELL_W - 2, ICON_CELL_H - 2, 11, 0xFFFFFF, 12);
        }
        int ix = x + (ICON_CELL_W - 52) / 2;
        gfx_shadow(&bs, ix, y + 6, 52, 52, 6, 70);
        draw_app_icon(ix, y + 6, 52, apps[i], 2);
        const char *n = apps[i]->name;
        int nw = gui_text_width(n);
        gfx_text(&bs, x + (ICON_CELL_W - nw) / 2 + 1, y + 63, n, 0x05070C, 0, 0);
        gfx_text(&bs, x + (ICON_CELL_W - nw) / 2, y + 62, n, 0xF2F5FF, 0, 0);
    }
}

static void draw_window(int id, int focused)
{
    struct window *w = &wins[id];
    if (!gfx_clip_hit(&bs, w->x - 18, w->y - 14, w->fw + 36, w->fh + 40)) return;
    gfx_shadow(&bs, w->x, w->y, w->fw, w->fh, 14, focused ? 125 : 70);
    gfx_rrect(&bs, w->x, w->y, w->fw, w->fh, WIN_R, focused ? gfx_mix(C_EDGE, acc, 120) : C_EDGE, 255);       /* рамка */
    gfx_rrect_m(&bs, w->x + 1, w->y + 1, w->fw - 2, TITLE_H - 1, WIN_R - 1, 3, focused ? 0x1C2542 : 0x151A2E, 255);
    if (focused) gfx_rrect_m(&bs, w->x + 1, w->y + 1, w->fw - 2, 14, WIN_R - 1, 3, 0xFFFFFF, 10);              /* блик */
    gfx_fill(&bs, w->x + 1, w->y + TITLE_H - 1, w->fw - 2, 1, 0x0B0F1C);
    if (focused) gfx_fill_a(&bs, w->x + 1, w->y + TITLE_H - 1, w->fw - 2, 1, acc, 170);
    draw_app_icon(w->x + 10, w->y + 7, 18, w->app, 1);
    gfx_text(&bs, w->x + 36, w->y + 8, w->title, focused ? 0xFFFFFF : GUI_C_DIM, 0, 0);

    int cx = w->x + w->fw - 1 - BTN_W, mx = cx - BTN_W, cy = w->y + 1, bh = TITLE_H - 1;
    if (hover_min == id) gfx_fill_a(&bs, mx, cy, BTN_W, bh, 0xFFFFFF, 30);
    if (hover_close == id) gfx_rrect_m(&bs, cx, cy, BTN_W, bh, WIN_R - 1, 2, 0xE81123, 255);
    uint32_t ic = focused || hover_close == id ? 0xFFFFFF : GUI_C_DIM;
    gfx_line(&bs, mx + BTN_W / 2 - 5, cy + bh / 2 + 3, mx + BTN_W / 2 + 5, cy + bh / 2 + 3, focused ? 0xFFFFFF : GUI_C_DIM);
    for (int k = 0; k < 2; k++) {                                                                             /* крестик, 2 px */
        gfx_line(&bs, cx + BTN_W / 2 - 5 + k, cy + bh / 2 - 5, cx + BTN_W / 2 + 5 + k, cy + bh / 2 + 5, ic);
        gfx_line(&bs, cx + BTN_W / 2 + 5 + k, cy + bh / 2 - 5, cx + BTN_W / 2 - 5 + k, cy + bh / 2 + 5, ic);
    }
    gfx_blit_round(&bs, w->x + 1, w->y + TITLE_H, w->cw.px, w->cw.w, w->cw.h, WIN_R - 1);
}

static void draw_taskbar(void)
{
    struct tl t;
    task_layout(&t);
    if (!gfx_clip_hit(&bs, 0, SH - TASK_H, SW, TASK_H)) return;
    gfx_shadow(&bs, t.px, t.py, t.pw, PILL_H, 8, 70);
    gfx_rrect(&bs, t.px - 1, t.py - 1, t.pw + 2, PILL_H + 2, 15, 0xFFFFFF, 34);
    gfx_rrect(&bs, t.px, t.py, t.pw, PILL_H, 14, 0x0C1022, 232);

    int sx = t.px + 8, sy = t.py + 4;                                     /* кнопка «Пуск»: четыре плитки */
    if (start_open) gfx_rrect(&bs, sx, sy, 40, 36, 9, 0xFFFFFF, 40);
    else if (hover_task == TASK_START) gfx_rrect(&bs, sx, sy, 40, 36, 9, 0xFFFFFF, 24);
    for (int i = 0; i < 4; i++)
        gfx_rrect(&bs, sx + 9 + (i % 2) * 12, sy + 7 + (i / 2) * 12, 10, 10, 3, i == 0 ? acc : gfx_mix(acc, 0xFFFFFF, 60 + i * 45), 255);

    int k = 0, top = top_win();
    for (int i = 0; i < GUI_MAX_WIN; i++) {
        if (!wins[i].used) continue;
        int bx = task_btn_x(&t, k++), by = t.py + 2;
        if (i == top) gfx_rrect(&bs, bx, by, 44, 40, 9, 0xFFFFFF, 26);
        else if (hover_task == i) gfx_rrect(&bs, bx, by, 44, 40, 9, 0xFFFFFF, 18);
        draw_app_icon(bx + 8, by + 5, 28, wins[i].app, 1);
        if (wins[i].min) gfx_rrect(&bs, bx + 8, by + 5, 28, 28, 7, 0x0C1022, 140);
        if (i == top) gfx_rrect(&bs, bx + 14, by + 36, 16, 3, 1, acc, 255);
        else gfx_rrect(&bs, bx + 19, by + 36, 6, 3, 1, GUI_C_DIM, 255);
    }

    gfx_shadow(&bs, t.tx, t.py, t.tw, PILL_H, 8, 60);                     /* системный трей */
    gfx_rrect(&bs, t.tx - 1, t.py - 1, t.tw + 2, PILL_H + 2, 15, 0xFFFFFF, 34);
    gfx_rrect(&bs, t.tx, t.py, t.tw, PILL_H, 14, cal_open ? 0x1A2142 : 0x0C1022, 232);
    if (hover_task == TASK_TRAY) gfx_rrect(&bs, t.tx + 4, t.py + 4, t.tw - 8, PILL_H - 8, 9, 0xFFFFFF, 20);
    char ts[16], ds[24];
    gui_snprintf(ts, sizeof ts, "%02d:%02d", tm.hour, tm.min);
    gui_snprintf(ds, sizeof ds, "%02d.%02d.%04d", tm.day, tm.mon, tm.year);
    int cx = t.tx + t.tw - 12 - 80;
    text_c(cx, t.py + 5, 80, ts, 0xFFFFFF);
    text_c(cx, t.py + 23, 80, ds, GUI_C_DIM);
    if (show_stats) {
        char hz[40];
        gui_snprintf(hz, sizeof hz, "%uHz %ufps", display_refresh_hz(), fps_now);
        gfx_text(&bs, t.tx + 12, t.py + 14, hz, GUI_C_GOOD, 0, 0);
    }
}

static void draw_tooltip(void)                       /* подпись окна над кнопкой панели задач */
{
    if (hover_task < 0 || !wins[hover_task].used || start_open || cal_open) return;
    struct tl t;
    task_layout(&t);
    int k = 0;
    for (int i = 0; i < hover_task; i++) k += wins[i].used;
    int w = gui_text_width(wins[hover_task].title) + 24, x = task_btn_x(&t, k) + 22 - w / 2, y = t.py - 40;
    if (!gfx_clip_hit(&bs, x - 4, y - 4, w + 8, 36)) return;
    gfx_shadow(&bs, x, y, w, 28, 6, 80);
    gfx_rrect(&bs, x, y, w, 28, 9, 0x0A0D1A, 240);
    text_c(x, y + 6, w, wins[hover_task].title, 0xFFFFFF);
}

static void draw_calendar(void)
{
    int x, y;
    cal_geom(&x, &y);
    if (!gfx_clip_hit(&bs, x - 20, y - 20, CAL_W + 40, CAL_H + 40)) return;
    gfx_shadow(&bs, x, y, CAL_W, CAL_H, 14, 120);
    gfx_rrect(&bs, x - 1, y - 1, CAL_W + 2, CAL_H + 2, 17, 0xFFFFFF, 30);
    gfx_rrect(&bs, x, y, CAL_W, CAL_H, 16, C_SHEET, 244);
    char s[48];
    gui_snprintf(s, sizeof s, "%02d:%02d:%02d", tm.hour, tm.min, tm.sec);
    gfx_text_big(&bs, x + 24, y + 18, s, 0xFFFFFF, 3);
    gui_snprintf(s, sizeof s, "%s, %d %s %d", WDAYS[tm.wday], tm.day, MONTHS[tm.mon - 1], tm.year);
    gfx_text(&bs, x + 26, y + 68, s, GUI_C_DIM, 0, 0);
    gfx_fill(&bs, x + 20, y + 90 - 2, CAL_W - 40, 1, C_EDGE);
    gui_snprintf(s, sizeof s, "%s %d", MONTHS[cal_m - 1], cal_y);
    text_c(x, y + 102, CAL_W, s, 0xFFFFFF);
    for (int i = 0; i < 2; i++) {
        int bx = i ? x + CAL_W - 46 : x + 14, by = y + 94;
        if (hover_cal == i + 1) gfx_rrect(&bs, bx, by, 32, 32, 9, 0xFFFFFF, 26);
        text_c(bx, by + 8, 32, i ? ">" : "<", 0xFFFFFF);
    }
    int cw = (CAL_W - 24) / 7, gx = x + 12;
    static const char *const hd[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    for (int i = 0; i < 7; i++) text_c(gx + i * cw, y + 138, cw, hd[i], i > 4 ? 0xE5707F : GUI_C_DIM);
    int first = (gui_weekday(cal_y, cal_m, 1) + 6) % 7, nd = gui_days_in_month(cal_y, cal_m);
    for (int d = 1; d <= nd; d++) {
        int idx = first + d - 1, r = idx / 7, c = idx % 7;
        int dx = gx + c * cw, dy = y + 162 + r * 34;
        int today = d == tm.day && cal_m == tm.mon && cal_y == tm.year;
        if (today) gfx_disc(&bs, dx + cw / 2, dy + 14, 15, acc, 255);
        gui_snprintf(s, sizeof s, "%d", d);
        text_c(dx, dy + 6, cw, s, today ? 0xFFFFFF : c > 4 ? 0xE5707F : GUI_C_TEXT);
    }
}

static void draw_start(void)
{
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);
    if (!gfx_clip_hit(&bs, x - 20, y - 20, w + 40, h + 40)) return;
    gfx_shadow(&bs, x, y, w, h, 16, 130);
    gfx_rrect(&bs, x - 1, y - 1, w + 2, h + 2, 19, 0xFFFFFF, 30);
    gfx_rrect(&bs, x, y, w, h, 18, C_SHEET, 244);

    gfx_rrect(&bs, x + 20, y + 20, w - 40, 44, 14, 0x1A2140, 255);       /* поле поиска */
    gfx_rrect(&bs, x + 20, y + 20, w - 40, 44, 14, acc, 40);
    gfx_disc(&bs, x + 44, y + 40, 7, GUI_C_DIM, 255);
    gfx_disc(&bs, x + 44, y + 40, 5, 0x1A2140, 255);
    gfx_line(&bs, x + 49, y + 45, x + 54, y + 50, GUI_C_DIM);
    gfx_line(&bs, x + 50, y + 45, x + 55, y + 50, GUI_C_DIM);
    if (start_ql) {
        gfx_text(&bs, x + 66, y + 34, start_q, 0xFFFFFF, 0, 0);
        gfx_fill(&bs, x + 66 + start_ql * 8, y + 35, 2, 14, acc);
    } else {
        gfx_fill(&bs, x + 66, y + 35, 2, 14, acc);
        gfx_text(&bs, x + 76, y + 34, "Type to search apps", 0x5C6786, 0, 0);
    }
    char s[40];
    gfx_text(&bs, x + 24, y + 76, start_ql ? "Results" : "Pinned", 0xFFFFFF, 0, 0);
    gui_snprintf(s, sizeof s, "%d app%s", start_n, start_n == 1 ? "" : "s");
    text_r(x, y + 76, w - 24, s, GUI_C_DIM);

    if (!start_n) text_c(x, y + 150, w, "No apps match your search", GUI_C_DIM);
    for (int slot = 0; slot < sm_slots && slot < start_n; slot++) {
        int tx, ty, tw, th;
        tile_rect(slot, &tx, &ty, &tw, &th);
        const struct gui_app *a = apps[start_list[slot]];
        if (slot == start_sel) {
            gfx_rrect(&bs, tx + 2, ty + 2, tw - 4, th - 4, 12, 0xFFFFFF, 22);
            gfx_rrect(&bs, tx + 2, ty + 2, tw - 4, th - 4, 12, acc, 40);
        }
        int is = th >= 88 ? 44 : 36;
        draw_app_icon(tx + (tw - is) / 2, ty + 8, is, a, is >= 44 ? 2 : 1);
        text_c(tx, ty + 8 + is + 8, tw, a->name, GUI_C_TEXT);
        for (int k = 0; k < GUI_MAX_WIN; k++)
            if (wins[k].used && wins[k].app == a) gfx_disc(&bs, tx + tw / 2 + is / 2 + 4, ty + 12, 3, GUI_C_GOOD, 255);
    }

    gfx_fill(&bs, x + 20, y + h - 60, w - 40, 1, C_EDGE);                 /* нижняя панель: пользователь и питание */
    const char *u = settings_get("sys.user");
    char ini[2] = {u && *u ? (char)((*u >= 'a' && *u <= 'z') ? *u - 32 : *u) : '?', 0};
    gfx_disc(&bs, x + 40, y + h - 30, 14, acc, 255);
    text_c(x + 26, y + h - 38, 28, ini, 0xFFFFFF);
    gfx_text(&bs, x + 64, y + h - 38, u && *u ? u : "user", 0xFFFFFF, 0, 0);
    static const char *const pl[3] = {"Exit GUI", "Restart", "Power off"};
    for (int i = 0; i < 3; i++) {
        int bx, by, bw, bh;
        power_btn_rect(i, &bx, &by, &bw, &bh);
        uint32_t bg = hover_pbtn == i ? (i == 2 ? 0xC0394D : 0x2C375E) : 0x1A2140;
        gfx_rrect(&bs, bx, by, bw, bh, 11, bg, 255);
        text_c(bx, by + 9, bw, pl[i], 0xFFFFFF);
    }
}

static void draw_ctx(void)
{
    int h = ctx_h();
    if (!gfx_clip_hit(&bs, ctx_x - 20, ctx_y - 20, CTX_W + 40, h + 40)) return;
    gfx_shadow(&bs, ctx_x, ctx_y, CTX_W, h, 12, 120);
    gfx_rrect(&bs, ctx_x - 1, ctx_y - 1, CTX_W + 2, h + 2, 13, 0xFFFFFF, 30);
    gfx_rrect(&bs, ctx_x, ctx_y, CTX_W, h, 12, C_SHEET, 246);
    for (int i = 0; i < NCTX; i++) {
        int y = ctx_item_y(i);
        if (CTX[i].sep) gfx_fill(&bs, ctx_x + 14, y - 5, CTX_W - 28, 1, C_EDGE);
        if (i == ctx_sel) gfx_rrect(&bs, ctx_x + 6, y, CTX_W - 12, 32, 8, acc, 120);
        gfx_text(&bs, ctx_x + 20, y + 8, CTX[i].label, i == ctx_sel ? 0xFFFFFF : GUI_C_TEXT, 0, 0);
    }
}

static void draw_toast(void)
{
    if (!toast_until) return;
    int w = gui_text_width(toast_msg) + 56, x = (SW - w) / 2, y = 16;
    if (!gfx_clip_hit(&bs, x - 16, y - 8, w + 32, 70)) return;
    gfx_shadow(&bs, x, y, w, 44, 10, 120);
    gfx_rrect(&bs, x - 1, y - 1, w + 2, 46, 15, 0xFFFFFF, 32);
    gfx_rrect(&bs, x, y, w, 44, 14, C_SHEET, 246);
    gfx_rrect(&bs, x + 12, y + 12, 4, 20, 2, acc, 255);
    gfx_text(&bs, x + 28, y + 14, toast_msg, 0xFFFFFF, 0, 0);
}

static void draw_banner(const char *msg)
{
    int w = gui_text_width(msg) + 32;
    int x = (SW - w) / 2;
    gfx_rrect(&bs, x, 70, w, 32, 10, GUI_C_WARN, 255);
    gfx_text(&bs, x + 16, 78, msg, 0x101010, 0, 0);
}

/* Перерисовывает только прямоугольник [x0,x1) x [y0,y1): на 4K полная пересборка кадра не успевала бы за 240 Гц. */
static void compose(int x0, int y0, int x1, int y1)
{
    gfx_set_clip(&bs, x0, y0, x1, y1);
    draw_wallpaper(x0, y0, x1, y1);
    draw_widget();
    draw_icons();
    for (int i = 0; i < nz; i++) if (!wins[zorder[i]].min) draw_window(zorder[i], i == nz - 1);
    if (top_win() < 0 && !start_open) {
        const char *m = "Press the Win key (or Esc) for the Start menu, right-click for more";
        text_c(0, (SH - TASK_H) / 2, SW, m, 0x9AA6C8);
    }
    draw_taskbar();
    draw_tooltip();
    if (cal_open) draw_calendar();
    if (start_open) draw_start();
    if (ctx_open) draw_ctx();
    draw_toast();
    if (mode == MODE_MOVE) draw_banner("MOVE: arrows move the window, Enter finishes");
    gfx_clear_clip();
}

/* ------------------------------------------------------------------ вывод на экран */
static void present(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = back + (long)y * SW + x0;
        uint32_t *d = (uint32_t *)((uint8_t *)fbi.base + (uint64_t)y * fbi.pitch) + x0;
        if (fbi.format == FB_BGRX) memcpy(d, s, (size_t)(x1 - x0) * 4);
        else for (int x = x0; x < x1; x++, s++, d++) *d = swap_rb(*s);               /* RGBX: поменять R и B */
    }
    cursor_paint();                                                                    /* present затёр курсор: рисуем заново */
}

static void draw_frame_now(void)                      /* немедленный полный кадр (перед выключением) */
{
    compose(0, 0, SW, SH);
    present(0, 0, SW, SH);
}

static void fb_clear(uint32_t rgb)
{
    uint32_t c = fbi.format == FB_BGRX ? rgb : swap_rb(rgb);
    for (uint32_t y = 0; y < fbi.height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)fbi.base + (uint64_t)y * fbi.pitch);
        for (uint32_t x = 0; x < fbi.width; x++) row[x] = c;
    }
}

/* ------------------------------------------------------------------ точки входа */
void gui_set_target_hz(unsigned hz)
{
    if (!hz) hz = display_refresh_hz();                                  /* из EDID; 0 — монитор не сообщил */
    if (!hz) hz = 240;                                                   /* кадр рисуется только при изменениях, так что запас дёшев */
    if (hz < 30) hz = 30;
    if (hz > 500) hz = 500;
    target_hz = hz;
}

void gui_init(const struct boot_info *bi)
{
    if (!bi->fb.base || bi->fb.format == FB_NONE) { pr_warn("no framebuffer, GUI disabled\n"); return; }
    fbi = bi->fb;
    SW = fbi.width > MAX_W ? MAX_W : (int)fbi.width;
    SH = fbi.height > MAX_H ? MAX_H : (int)fbi.height;
    if (SW < 640 || SH < 480) { pr_warn("screen %dx%d too small for GUI (need 640x480)\n", SW, SH); return; }

    uint64_t bpages = ((uint64_t)SW * SH * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t spages = ((uint64_t)GUI_MAX_WIN * GUI_SURF_PX * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t b = pmm_alloc_contig(bpages), sp = pmm_alloc_contig(spages), wp = pmm_alloc_contig(bpages);
    if (!b || !sp) {
        pr_warn("not enough contiguous memory for GUI buffers (%lu + %lu KiB)\n", bpages * 4, spages * 4);
        if (b) for (uint64_t i = 0; i < bpages; i++) pmm_free(b + i * PAGE_SIZE);
        if (sp) for (uint64_t i = 0; i < spages; i++) pmm_free(sp + i * PAGE_SIZE);
        if (wp) for (uint64_t i = 0; i < bpages; i++) pmm_free(wp + i * PAGE_SIZE);
        return;
    }
    back = (uint32_t *)b;
    surf_mem = (uint32_t *)sp;
    wall = (uint32_t *)wp;                                               /* NULL — не страшно: будут простые обои */
    bs = (struct gfx_surf){back, SW, SH};
    cur_scale = SH >= 2000 ? 2 : 1;                                      /* на 4K курсор вдвое крупнее, как масштабирование Windows */
    gui_set_target_hz(0);
    ready = 1;
    pr_info("server ready: %dx%d, %d apps, %u Hz target, backbuffer %lu MiB\n", SW, SH, NAPPS, target_hz, bpages * 4 >> 10);
}

int gui_run(void)
{
    if (!ready) return -1;
    memset(wins, 0, sizeof wins);
    nz = 0; cascade = 0; mode = MODE_NORMAL; quit = 0;
    start_open = ctx_open = cal_open = 0; toast_until = 0;
    rqh = rqt = 0;
    icon_sel = -1; icon_click_idx = -1; drag_win = capture_win = -1;
    hover_close = hover_min = hover_tile = hover_pbtn = hover_ctx = -1; hover_task = TASK_NONE; hover_cal = 0;
    btn_state = 0; acc_dx = acc_dy = rem_x = rem_y = 0;
    cur_x = SW / 2; cur_y = SH / 2; cur_drawn = 0;
    gui_set_target_hz(target_hz);                                        /* заново из EDID/`hz N`, если изменили */
    gui_get_time(&tm);
    load_theme();

    memset(back, 0, (size_t)SW * SH * 4);
    struct mouse_pkt junk;
    while (mouse_pop(&junk) == 0) {}                                     /* накопившееся до запуска GUI */

    console_fb_mute(1);                       /* логи ядра — только в serial, чтобы не портить кадр */
    fb_clear(0x000000);
    mark_all();
    spawn(0);                                 /* терминал открывается сам */

    uint64_t frame_us = 1000000ull / target_hz, next_frame = time_now_us(), sec_mark = next_frame, idle_us = 0;
    fps_count = fps_now = 0;

    while (!quit) {
        frame_us = 1000000ull / target_hz;        /* `hz N` и Display Test действуют на лету */
        int busy = 0, c;
        input_poll();
        for (int n = 0; n < 64 && (c = input_getc()) >= 0; n++) { handle_key(c); busy = 1; }

        uint64_t now_us = time_now_us();
        process_mouse(now_us);

        uint32_t now = (uint32_t)time_uptime_ms();
        run_timers(now);
        if (dispatch_events()) busy = 1;
        if (drain_requests()) busy = 1;
        if (toast_until && now >= toast_until) { toast_until = 0; mark(0, 0, SW, 90); }

        if (now_us - sec_mark >= 1000000) {                              /* раз в секунду: часы, fps и загрузка */
            busy_pct = (unsigned)(100 - (idle_us * 100 / (now_us - sec_mark) > 100 ? 100 : idle_us * 100 / (now_us - sec_mark)));
            idle_us = 0;
            sec_mark = now_us; fps_now = fps_count; fps_count = 0;
            int old_min = tm.min;
            gui_get_time(&tm);
            mark(0, SH - TASK_H, SW, TASK_H);
            if (tm.min != old_min) mark(SW - 420, 10, 420, 120);
            if (cal_open) { int cx, cy; cal_geom(&cx, &cy); mark(cx, cy, CAL_W, 90); }
        }

        if (dirty) {
            now_us = time_now_us();
            if (now_us >= next_frame) {
                int x0 = dx0, y0 = dy0, x1 = dx1, y1 = dy1;              /* прямоугольник фиксируем до compose: сервер может пометить ещё */
                dirty = 0;
                compose(x0, y0, x1, y1);
                present(x0, y0, x1, y1);
                fps_count++;
                next_frame += frame_us;
                if (next_frame < now_us) next_frame = now_us + frame_us;   /* отстали больше чем на кадр: не догоняем пачкой */
                busy = 1;
            } else if (next_frame - now_us < 1500) {
                busy = 1;                                                /* до кадра меньше тика PIT: ждём вращением, а не hlt */
                __asm__ volatile("pause");
            }
        }
        if (!busy) {                                                     /* пробудит IRQ: PIT 1 кГц, PS/2-мышь, клавиатура */
            uint64_t t0 = time_now_us();
            cpu_idle();
            idle_us += time_now_us() - t0;
        }
    }

    for (int i = 0; i < GUI_MAX_WIN; i++) if (wins[i].used) close_win(i);
    cur_drawn = 0;
    fb_clear(0x0B1020);
    console_fb_mute(0);
    kprintf("\f");                           /* fbcon: очистить и вернуть курсор в начало */
    return 0;
}

/* ------------------------------------------------------------------ сервисы для приложений */
int gui_app_count(void) { return NAPPS; }
const char *gui_app_name(int i) { return i >= 0 && i < NAPPS ? apps[i]->name : 0; }
unsigned gui_fps(void) { return fps_now; }
unsigned gui_target_hz(void) { return target_hz; }
unsigned gui_cpu_busy(void) { return busy_pct; }

int gui_launch(const char *name)
{
    if (!name || !*name) return -1;
    for (int i = 0; i < NAPPS; i++) {
        const char *a = apps[i]->name, *b = name;
        while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
        if (!*b) return spawn(i) >= 0 ? 0 : -1;
    }
    return -1;
}

static int nth_win(int i)                       /* i-е существующее окно -> индекс слота или -1 */
{
    for (int k = 0; k < GUI_MAX_WIN; k++) if (wins[k].used && i-- == 0) return k;
    return -1;
}

int gui_task_count(void) { int n = 0; for (int k = 0; k < GUI_MAX_WIN; k++) n += wins[k].used; return n; }

int gui_task_info(int i, struct gui_task *o)
{
    int k = nth_win(i);
    if (k < 0) return -1;
    memset(o, 0, sizeof *o);
    memcpy(o->name, wins[k].title, sizeof o->name);
    o->name[sizeof o->name - 1] = 0;
    o->color = wins[k].app->color;
    o->surf_kib = (uint32_t)(wins[k].cw.w * wins[k].cw.h * 4 / 1024);
    o->up_s = ((uint32_t)time_uptime_ms() - wins[k].born_ms) / 1000;
    o->focused = k == top_win();
    o->minimized = (uint8_t)wins[k].min;
    return 0;
}

void gui_task_end(int i) { int k = nth_win(i); if (k >= 0) close_win(k); }

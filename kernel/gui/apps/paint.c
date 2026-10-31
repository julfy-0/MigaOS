/* Paint: ЛКМ рисует, ПКМ стирает, клик по палитре выбирает цвет, + - размер, C очистка, E ластик. */
#include <kernel/gui.h>

#define TOOLH 40
#define WHITE 0xFFFFFF
static const uint32_t pal[16] = {0x000000, 0xFFFFFF, 0xE5566D, 0xF2B84B, 0xFFE14D, 0x4CD08A, 0x2FBFBF, 0x4C8DFF,
                                 0x8A4FD8, 0xD84FA8, 0x7A4A2B, 0x8B95AD, 0x39445F, 0x1B2236, 0xA8E060, 0xFF8A4C};
static uint32_t color; static int bsz, drawing, lx, ly;

static void toolbar(struct gui_win *w)
{
    gui_fill(w, 0, 0, w->w, TOOLH, GUI_C_PANEL);
    for (int i = 0; i < 16; i++) {
        gui_fill(w, 8 + i * 26, 9, 22, 22, pal[i]);
        gui_rect(w, 8 + i * 26, 9, 22, 22, pal[i] == color ? GUI_C_WARN : GUI_C_LINE);
    }
    gui_printf(w, 440, 4, GUI_C_TEXT, "size %d", bsz);
    gui_text(w, 440, 22, "+/- C=clear E=erase", GUI_C_DIM);
    gui_damage(w, 0, 0, w->w, TOOLH);
}

static void stamp(struct gui_win *w, int x, int y, uint32_t c)
{
    int x0 = x - bsz / 2, y0 = y - bsz / 2, x1 = x0 + bsz, y1 = y0 + bsz;
    if (y0 < TOOLH) y0 = TOOLH;
    if (x0 < 0) x0 = 0;
    if (x1 > w->w) x1 = w->w;
    if (y1 > w->h) y1 = w->h;
    if (x1 > x0 && y1 > y0) gui_fill(w, x0, y0, x1 - x0, y1 - y0, c);
}

static void seg(struct gui_win *w, int ax, int ay, int bx, int by, uint32_t c)
{
    int dx = bx - ax, dy = by - ay, n = dx < 0 ? -dx : dx, m = dy < 0 ? -dy : dy;
    if (m > n) n = m;
    for (int i = 0; i <= n; i++) stamp(w, n ? ax + dx * i / n : ax, n ? ay + dy * i / n : ay, c);
    int h = bsz / 2 + 1, x0 = (ax < bx ? ax : bx) - h, y0 = (ay < by ? ay : by) - h;
    int x1 = (ax > bx ? ax : bx) + h + 1, y1 = (ay > by ? ay : by) + h + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < TOOLH) y0 = TOOLH;
    if (x1 > w->w) x1 = w->w;
    if (y1 > w->h) y1 = w->h;
    if (x1 > x0 && y1 > y0) gui_damage(w, x0, y0, x1 - x0, y1 - y0);
}

static void canvas_clear(struct gui_win *w) { gui_fill(w, 0, TOOLH, w->w, w->h - TOOLH, WHITE); gui_present(w); }

static void start(struct gui_win *w)
{
    color = 0x000000; bsz = 4; drawing = 0;
    gui_clear(w, WHITE); toolbar(w); gui_present(w);
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type == GUI_EV_KEY) {
        char k = (char)e->key;
        if (k == '+' || k == '=') { if (bsz < 40) bsz += 2; toolbar(w); }
        else if (k == '-') { if (bsz > 2) bsz -= 2; toolbar(w); }
        else if (k == 'c' || k == 'C') { canvas_clear(w); }
        else if (k == 'e' || k == 'E') { color = WHITE; toolbar(w); }
        return;
    }
    if (e->type != GUI_EV_MOUSE) return;
    uint32_t pen = (e->buttons & 2) ? WHITE : color;
    if (e->mtype == GUI_MOUSE_DOWN) {
        if (e->my < TOOLH) {
            int i = (e->mx - 8) / 26;
            if (e->mx >= 8 && i >= 0 && i < 16) { color = pal[i]; toolbar(w); }
            return;
        }
        drawing = 1; lx = e->mx; ly = e->my;
        seg(w, lx, ly, lx, ly, pen);
    } else if (e->mtype == GUI_MOUSE_MOVE && drawing && (e->buttons & 3)) {
        seg(w, lx, ly, e->mx, e->my, pen);
        lx = e->mx; ly = e->my;
    } else if (e->mtype == GUI_MOUSE_UP) drawing = 0;
}

const struct gui_app app_paint = {
    .name = "Paint", .glyph = "Pt", .color = 0xD84FA8, .w = 600, .h = 440, .tick_ms = 0,
    .start = start, .event = ev,
};

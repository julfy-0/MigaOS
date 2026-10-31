/* Клиентская часть MigaGUI: то, что вызывают приложения.
 * Пиксельные операции идут прямо в общую поверхность окна, остальное — запросы серверу. */
#include "gui_priv.h"
#include <miga/string.h>

#define SURF(w) ((struct gfx_surf){(w)->px, (w)->w, (w)->h})

void gui_clear(struct gui_win *w, uint32_t c) { struct gfx_surf s = SURF(w); gfx_fill(&s, 0, 0, w->w, w->h, c); }
void gui_fill(struct gui_win *w, int x, int y, int cw, int ch, uint32_t c) { struct gfx_surf s = SURF(w); gfx_fill(&s, x, y, cw, ch, c); }
void gui_rect(struct gui_win *w, int x, int y, int cw, int ch, uint32_t c) { struct gfx_surf s = SURF(w); gfx_rect(&s, x, y, cw, ch, c); }
void gui_line(struct gui_win *w, int x0, int y0, int x1, int y1, uint32_t c) { struct gfx_surf s = SURF(w); gfx_line(&s, x0, y0, x1, y1, c); }
void gui_text(struct gui_win *w, int x, int y, const char *t, uint32_t fg) { struct gfx_surf s = SURF(w); gfx_text(&s, x, y, t, fg, 0, 0); }
void gui_text_bg(struct gui_win *w, int x, int y, const char *t, uint32_t fg, uint32_t bg) { struct gfx_surf s = SURF(w); gfx_text(&s, x, y, t, fg, bg, 1); }
void gui_text_big(struct gui_win *w, int x, int y, const char *t, uint32_t fg, int sc) { struct gfx_surf s = SURF(w); gfx_text_big(&s, x, y, t, fg, sc); }
void gui_rrect(struct gui_win *w, int x, int y, int cw, int ch, int r, uint32_t c) { struct gfx_surf s = SURF(w); gfx_rrect(&s, x, y, cw, ch, r, c, 255); }
void gui_disc(struct gui_win *w, int cx, int cy, int r, uint32_t c) { struct gfx_surf s = SURF(w); gfx_disc(&s, cx, cy, r, c, 255); }
int  gui_text_width(const char *t) { return (int)strlen(t) * GUI_CHAR_W; }

void gui_bar(struct gui_win *w, int x, int y, int cw, int ch, uint64_t val, uint64_t max, uint32_t fg, uint32_t bg)
{
    struct gfx_surf s = SURF(w);
    gfx_fill(&s, x, y, cw, ch, bg);
    if (max) {
        if (val > max) val = max;
        gfx_fill(&s, x, y, (int)((uint64_t)cw * val / max), ch, fg);
    }
    gfx_rect(&s, x, y, cw, ch, GUI_C_LINE);
}

void gui_printf(struct gui_win *w, int x, int y, uint32_t fg, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    gui_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    gui_text(w, x, y, buf, fg);
}

static void post(struct gui_win *w, uint8_t type, int x, int y, int cw, int ch, const char *s)
{
    struct gui_req r;
    memset(&r, 0, sizeof r);
    r.type = type; r.win = (uint8_t)w->id; r.x = x; r.y = y; r.w = cw; r.h = ch;
    if (s) for (unsigned i = 0; i < sizeof r.s - 1 && s[i]; i++) r.s[i] = s[i];
    gui_srv_post(&r);
}

void gui_present(struct gui_win *w) { post(w, GUI_RQ_DAMAGE, 0, 0, 0, 0, 0); }
void gui_damage(struct gui_win *w, int x, int y, int cw, int ch) { post(w, GUI_RQ_DAMAGE, x, y, cw, ch, 0); }
void gui_set_title(struct gui_win *w, const char *t) { post(w, GUI_RQ_TITLE, 0, 0, 0, 0, t); }
void gui_set_tick(struct gui_win *w, unsigned ms) { post(w, GUI_RQ_TICK, (int)ms, 0, 0, 0, 0); }
void gui_notify(struct gui_win *w, const char *msg) { post(w, GUI_RQ_NOTIFY, 0, 0, 0, 0, msg); }
void gui_close(struct gui_win *w) { post(w, GUI_RQ_CLOSE, 0, 0, 0, 0, 0); }

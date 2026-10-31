/* Внутренний заголовок GUI: протокол клиент→сервер и графические примитивы.
 * Приложения его не включают. */
#pragma once
#include <kernel/gui.h>

/* ---- протокол клиент → сервер ---- */
enum gui_req_type {
    GUI_RQ_DAMAGE = 1,  /* x,y,w,h в координатах клиентской области; w<=0 = всё окно */
    GUI_RQ_TITLE,       /* s = новый заголовок */
    GUI_RQ_CLOSE,
    GUI_RQ_TICK,        /* x = период таймера, мс (0 = выключить) */
    GUI_RQ_NOTIFY,      /* s = текст всплывающего уведомления */
};

struct gui_req {
    uint8_t type;       /* enum gui_req_type */
    uint8_t win;
    uint16_t _pad;
    int32_t x, y, w, h;
    char    s[24];
};

#define GUI_REQQ 64
#define GUI_EVQ  32

int gui_srv_post(const struct gui_req *r);   /* server.c: положить запрос в очередь */

/* ---- графика на произвольной поверхности 0x00RRGGBB ---- */
struct gfx_surf { uint32_t *px; int w, h; };

void gfx_fill(const struct gfx_surf *s, int x, int y, int w, int h, uint32_t c);
void gfx_rect(const struct gfx_surf *s, int x, int y, int w, int h, uint32_t c);
void gfx_line(const struct gfx_surf *s, int x0, int y0, int x1, int y1, uint32_t c);
void gfx_char(const struct gfx_surf *s, int x, int y, char ch, uint32_t fg, uint32_t bg, int opaque);
void gfx_text(const struct gfx_surf *s, int x, int y, const char *str, uint32_t fg, uint32_t bg, int opaque);
void gfx_text_big(const struct gfx_surf *s, int x, int y, const char *str, uint32_t fg, int scale);
void gfx_blit(const struct gfx_surf *dst, int dx, int dy, const uint32_t *src, int sw, int sh);

/* Сглаженные примитивы с прозрачностью (a = 0..255), цвета 0x00RRGGBB. mask углов rrect_m: 1=верх-лево 2=верх-право 4=низ-лево 8=низ-право */
void gfx_fill_a(const struct gfx_surf *s, int x, int y, int w, int h, uint32_t c, int a);
void gfx_rrect(const struct gfx_surf *s, int x, int y, int w, int h, int r, uint32_t c, int a);
void gfx_rrect_m(const struct gfx_surf *s, int x, int y, int w, int h, int r, int mask, uint32_t c, int a);
void gfx_disc(const struct gfx_surf *s, int cx, int cy, int r, uint32_t c, int a);
void gfx_vgrad(const struct gfx_surf *s, int x, int y, int w, int h, uint32_t c0, uint32_t c1);
void gfx_shadow(const struct gfx_surf *s, int x, int y, int w, int h, int spread, int a);
uint32_t gfx_mix(uint32_t c0, uint32_t c1, int a);
void gfx_glow(const struct gfx_surf *s, int cx, int cy, int r, uint32_t c, int peak);
void gfx_blit_round(const struct gfx_surf *dst, int dx, int dy, const uint32_t *src, int sw, int sh, int r);

/* Клип-прямоугольник для ОДНОЙ поверхности (backbuffer сервера): композитор перерисовывает только грязную область.
 * На поверхности окон клип не действует. */
void gfx_set_clip(const struct gfx_surf *s, int x0, int y0, int x1, int y1);
void gfx_clear_clip(void);
int  gfx_clip_hit(const struct gfx_surf *s, int x, int y, int w, int h);   /* прямоугольник пересекает клип? */

/* ---- курсор мыши (cursor.c) ---- */
#define CUR_W 12
#define CUR_H 19
const uint8_t *cursor_shape(void);        /* CUR_W*CUR_H: 0 прозрачно, 1 контур, 2 заливка */

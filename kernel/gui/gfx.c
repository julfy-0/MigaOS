/* Графические примитивы MigaGUI. Все функции безопасны к выходу за границы поверхности. */
#include "gui_priv.h"
#include <miga/string.h>
#include "../../drivers/video/fbcon/font8x16.h"   /* тот же шрифт 8x16, что у fbcon (static const, дублирования символов нет) */

static const uint32_t *clip_px;                   /* поверхность, на которую действует клип (или NULL) */
static int clip_x0, clip_y0, clip_x1, clip_y1;

void gfx_set_clip(const struct gfx_surf *s, int x0, int y0, int x1, int y1)
{
    clip_px = s->px; clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
}
void gfx_clear_clip(void) { clip_px = 0; }

/* Границы рисования на поверхности: сама поверхность, пересечённая с клипом. */
static void bounds(const struct gfx_surf *s, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h;
    if (clip_px && clip_px == s->px) {
        if (clip_x0 > *x0) *x0 = clip_x0;
        if (clip_y0 > *y0) *y0 = clip_y0;
        if (clip_x1 < *x1) *x1 = clip_x1;
        if (clip_y1 < *y1) *y1 = clip_y1;
    }
}

int gfx_clip_hit(const struct gfx_surf *s, int x, int y, int w, int h)
{
    int bx0, by0, bx1, by1;
    bounds(s, &bx0, &by0, &bx1, &by1);
    return x < bx1 && x + w > bx0 && y < by1 && y + h > by0;
}

void gfx_fill(const struct gfx_surf *s, int x, int y, int w, int h, uint32_t c)
{
    int bx0, by0, bx1, by1;
    bounds(s, &bx0, &by0, &bx1, &by1);
    int x1 = x + w, y1 = y + h;
    if (x < bx0) x = bx0;
    if (y < by0) y = by0;
    if (x1 > bx1) x1 = bx1;
    if (y1 > by1) y1 = by1;
    if (x >= x1 || y >= y1) return;
    for (int j = y; j < y1; j++) {
        uint32_t *p = s->px + (long)j * s->w + x;
        for (int i = x; i < x1; i++) *p++ = c;
    }
}

void gfx_rect(const struct gfx_surf *s, int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    gfx_fill(s, x, y, w, 1, c);
    gfx_fill(s, x, y + h - 1, w, 1, c);
    gfx_fill(s, x, y, 1, h, c);
    gfx_fill(s, x + w - 1, y, 1, h, c);
}

void gfx_line(const struct gfx_surf *s, int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1; /* dy <= 0 */
    int err = dx + dy;
    int bx0, by0, bx1, by1;
    bounds(s, &bx0, &by0, &bx1, &by1);
    for (;;) {
        if (x0 >= bx0 && y0 >= by0 && x0 < bx1 && y0 < by1) s->px[(long)y0 * s->w + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_char(const struct gfx_surf *s, int x, int y, char ch, uint32_t fg, uint32_t bg, int opaque)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    int bx0, by0, bx1, by1;
    bounds(s, &bx0, &by0, &bx1, &by1);
    if (x + FONT_W <= bx0 || y + FONT_H <= by0 || x >= bx1 || y >= by1) return;
    const uint8_t *g = font8x16[ch - FONT_FIRST];
    for (int j = 0; j < FONT_H; j++) {
        int py = y + j;
        if (py < by0 || py >= by1) continue;
        uint32_t *row = s->px + (long)py * s->w;
        for (int i = 0; i < FONT_W; i++) {
            int px = x + i;
            if (px < bx0 || px >= bx1) continue;
            if (g[j] & (0x80 >> i)) row[px] = fg;
            else if (opaque) row[px] = bg;
        }
    }
}

void gfx_text(const struct gfx_surf *s, int x, int y, const char *str, uint32_t fg, uint32_t bg, int opaque)
{
    for (; *str; str++, x += FONT_W) gfx_char(s, x, y, *str, fg, bg, opaque);
}

void gfx_text_big(const struct gfx_surf *s, int x, int y, const char *str, uint32_t fg, int scale)
{
    if (scale < 1) scale = 1;
    for (; *str; str++, x += FONT_W * scale) {
        char ch = *str;
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        const uint8_t *g = font8x16[ch - FONT_FIRST];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (g[j] & (0x80 >> i)) gfx_fill(s, x + i * scale, y + j * scale, scale, scale, fg);
    }
}

void gfx_blit(const struct gfx_surf *dst, int dx, int dy, const uint32_t *src, int sw, int sh)
{
    int bx0, by0, bx1, by1;
    bounds(dst, &bx0, &by0, &bx1, &by1);
    int x0 = 0, y0 = 0, x1 = sw, y1 = sh;
    if (dx + x0 < bx0) x0 = bx0 - dx;
    if (dy + y0 < by0) y0 = by0 - dy;
    if (dx + x1 > bx1) x1 = bx1 - dx;
    if (dy + y1 > by1) y1 = by1 - dy;
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++)
        memcpy(dst->px + (long)(dy + j) * dst->w + dx + x0, src + (long)j * sw + x0, (size_t)(x1 - x0) * 4);
}

/* ================================================================== сглаживание, прозрачность, скругления
 * Цвета логические 0x00RRGGBB, альфа 0..255 (255 = непрозрачно). Всё уважает клип поверхности, как и выше. */
static inline uint32_t al256(int a) { return a <= 0 ? 0 : a >= 255 ? 256 : (uint32_t)a + ((uint32_t)a >> 7); }

static inline uint32_t blend(uint32_t d, uint32_t s, uint32_t a)      /* a = 0..256 */
{
    uint32_t ia = 256 - a;
    uint32_t rb = ((((s & 0xFF00FF) * a) + ((d & 0xFF00FF) * ia)) >> 8) & 0xFF00FF;
    uint32_t g  = ((((s & 0x00FF00) * a) + ((d & 0x00FF00) * ia)) >> 8) & 0x00FF00;
    return rb | g;
}

static uint32_t isqrt32(uint32_t n)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > n) b >>= 2;
    while (b) {
        if (n >= r + b) { n -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

static void put_a(const struct gfx_surf *s, int x, int y, uint32_t c, uint32_t a, int bx0, int by0, int bx1, int by1)
{
    if (x < bx0 || y < by0 || x >= bx1 || y >= by1 || !a) return;
    uint32_t *p = s->px + (long)y * s->w + x;
    *p = blend(*p, c, a);
}

static void span_a(const struct gfx_surf *s, int y, int x0, int x1, uint32_t c, uint32_t a, int bx0, int by0, int bx1, int by1)
{
    if (y < by0 || y >= by1) return;
    if (x0 < bx0) x0 = bx0;
    if (x1 > bx1) x1 = bx1;
    uint32_t *p = s->px + (long)y * s->w;
    if (a >= 256) { for (int x = x0; x < x1; x++) p[x] = c; }
    else for (int x = x0; x < x1; x++) p[x] = blend(p[x], c, a);
}

void gfx_fill_a(const struct gfx_surf *s, int x, int y, int w, int h, uint32_t c, int a)
{
    int bx0, by0, bx1, by1;
    bounds(s, &bx0, &by0, &bx1, &by1);
    uint32_t al = al256(a);
    for (int j = y; j < y + h; j++) span_a(s, j, x, x + w, c, al, bx0, by0, bx1, by1);
}

static int cov_corner(int px, int py, int r)        /* покрытие пикселя угла радиуса r (px,py — от внешнего угла), 0..255 */
{
    int ex = 2 * (r - px) - 1, ey = 2 * (r - py) - 1;
    int d2 = ex * ex + ey * ey;
    if (d2 <= (2 * r - 2) * (2 * r - 2)) return 255;
    if (d2 >= (2 * r + 2) * (2 * r + 2)) return 0;
    int v = (32 * r - (int)isqrt32((uint32_t)d2 << 8)) * 8 + 127;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

void gfx_rrect_m(const struct gfx_surf *s, int x, int y, int w, int h, int r, int mask, uint32_t c, int a)
{
    if (w <= 0 || h <= 0 || a <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int bx0, by0, bx1, by1;
    bounds(s, &bx0, &by0, &bx1, &by1);
    uint32_t al = al256(a);
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < by0 || yy >= by1) continue;
        int top = j < r, bot = j >= h - r, k = top ? j : bot ? h - 1 - j : -1;
        int lr = k >= 0 && ((top && (mask & 1)) || (bot && (mask & 4)));
        int rr = k >= 0 && ((top && (mask & 2)) || (bot && (mask & 8)));
        int xs = x, xe = x + w;
        if (lr) { for (int i = 0; i < r; i++) { int cv = cov_corner(i, k, r); if (cv) put_a(s, x + i, yy, c, al * (uint32_t)cv / 255, bx0, by0, bx1, by1); } xs = x + r; }
        if (rr) { for (int i = 0; i < r; i++) { int cv = cov_corner(i, k, r); if (cv) put_a(s, x + w - 1 - i, yy, c, al * (uint32_t)cv / 255, bx0, by0, bx1, by1); } xe = x + w - r; }
        span_a(s, yy, xs, xe, c, al, bx0, by0, bx1, by1);
    }
}

void gfx_rrect(const struct gfx_surf *s, int x, int y, int w, int h, int r, uint32_t c, int a)
{
    gfx_rrect_m(s, x, y, w, h, r, 15, c, a);
}

static int cov_disc(int dx, int dy, int r)
{
    int d2 = dx * dx + dy * dy;
    if (d2 >= (r + 1) * (r + 1)) return 0;
    if (r > 0 && d2 <= (r - 1) * (r - 1)) return 255;
    int v = (r * 16 - (int)isqrt32((uint32_t)d2 << 8)) * 16 + 127;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

void gfx_disc(const struct gfx_surf *s, int cx, int cy, int r, uint32_t c, int a)
{
    if (a <= 0 || r < 0) return;
    int bx0, by0, bx1, by1;
    bounds(s, &bx0, &by0, &bx1, &by1);
    uint32_t al = al256(a);
    for (int dy = -r - 1; dy <= r + 1; dy++) {
        int y = cy + dy;
        if (y < by0 || y >= by1) continue;
        int in = -1;
        if (r >= 1 && dy * dy <= (r - 1) * (r - 1)) in = (int)isqrt32((uint32_t)((r - 1) * (r - 1) - dy * dy));
        if (in >= 0) span_a(s, y, cx - in, cx + in + 1, c, al, bx0, by0, bx1, by1);
        for (int dx = in >= 0 ? in + 1 : 0; dx <= r + 1; dx++) {
            int cv = cov_disc(dx, dy, r);
            if (!cv) continue;
            uint32_t aa = al * (uint32_t)cv / 255;
            put_a(s, cx + dx, y, c, aa, bx0, by0, bx1, by1);
            if (dx) put_a(s, cx - dx, y, c, aa, bx0, by0, bx1, by1);
        }
    }
}

void gfx_vgrad(const struct gfx_surf *s, int x, int y, int w, int h, uint32_t c0, uint32_t c1)
{
    for (int j = 0; j < h; j++) {
        int t = h > 1 ? j * 255 / (h - 1) : 0;
        uint32_t c = 0;
        for (int sh = 0; sh < 24; sh += 8)
            c |= ((((c0 >> sh) & 255) * (255 - t) + ((c1 >> sh) & 255) * t) / 255) << sh;
        gfx_fill(s, x, y + j, w, 1, c);
    }
}

/* Мягкая тень: кольца вокруг прямоугольника с квадратично затухающей прозрачностью (смещены вниз на 6 px). */
void gfx_shadow(const struct gfx_surf *s, int x, int y, int w, int h, int spread, int a)
{
    int bx0, by0, bx1, by1;
    bounds(s, &bx0, &by0, &bx1, &by1);
    for (int i = spread; i >= 1; i--) {
        uint32_t al = al256(a * (spread + 1 - i) * (spread + 1 - i) / (spread * spread));
        int ox = x - i, oy = y - i + 6, ow = w + 2 * i, oh = h + 2 * i, q = i / 2;
        span_a(s, oy, ox + q, ox + ow - q, 0, al, bx0, by0, bx1, by1);
        span_a(s, oy + oh - 1, ox + q, ox + ow - q, 0, al, bx0, by0, bx1, by1);
        for (int j = oy + q; j < oy + oh - q; j++) {
            put_a(s, ox, j, 0, al, bx0, by0, bx1, by1);
            put_a(s, ox + ow - 1, j, 0, al, bx0, by0, bx1, by1);
        }
    }
}

/* Копирует поверхность клиента, скругляя два нижних угла радиусом r: угловые пиксели подмешиваются к тому,
 * что уже нарисовано под ними (рамка окна), по покрытию дуги. */
void gfx_blit_round(const struct gfx_surf *dst, int dx, int dy, const uint32_t *src, int sw, int sh, int r)
{
    if (r > sh) r = sh;
    if (r > sw / 2) r = sw / 2;
    gfx_blit(dst, dx, dy, src, sw, sh - r);
    int bx0, by0, bx1, by1;
    bounds(dst, &bx0, &by0, &bx1, &by1);
    for (int k = 0; k < r; k++) {
        int row = sh - r + k, yy = dy + row;
        if (yy < by0 || yy >= by1) continue;
        const uint32_t *sp = src + (long)row * sw;
        uint32_t *dp = dst->px + (long)yy * dst->w;
        for (int i = 0; i < sw; i++) {
            int xx = dx + i;
            if (xx < bx0 || xx >= bx1) continue;
            int e = i < r ? i : (i >= sw - r ? sw - 1 - i : -1);          /* расстояние до бокового края в зоне угла */
            if (e < 0) { dp[xx] = sp[i]; continue; }
            int cv = cov_corner(e, r - 1 - k, r);
            if (cv >= 255) dp[xx] = sp[i];
            else if (cv) dp[xx] = blend(dp[xx], sp[i], al256(cv));
        }
    }
}

/* Мягкое радиальное свечение (для обоев): пик прозрачности peak в центре, спад к нулю на радиусе r. */
void gfx_glow(const struct gfx_surf *s, int cx, int cy, int r, uint32_t c, int peak)
{
    uint32_t r2 = (uint32_t)r * (uint32_t)r;
    for (int y = cy - r; y < cy + r; y++) {
        if (y < 0 || y >= s->h) continue;
        uint32_t *row = s->px + (long)y * s->w;
        for (int x = cx - r; x < cx + r; x++) {
            if (x < 0 || x >= s->w) continue;
            uint32_t d2 = (uint32_t)((x - cx) * (x - cx) + (y - cy) * (y - cy));
            if (d2 >= r2) continue;
            uint32_t t = 256 - (uint32_t)(((uint64_t)d2 << 8) / r2);
            row[x] = blend(row[x], c, (t * t * (uint32_t)peak) >> 16);
        }
    }
}

/* Смешивание двух цветов: 0 = c0, 255 = c1. */
uint32_t gfx_mix(uint32_t c0, uint32_t c1, int a)
{
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return blend(c0, c1, (uint32_t)a * 256 / 255);
}

/* Текстовая консоль поверх линейного фреймбуфера (формат от UEFI GOP, 32 bpp). */
#define MIGA_TAG "fbcon"
#include <miga/driver.h>
#include <miga/console.h>
#include <miga/string.h>
#include <miga/log.h>
#include "font8x16.h"

#define FG 0xD0D6E0
#define BG 0x0B1020

static struct {
    uint8_t *base;
    uint32_t w, h, pitch, fmt;
    uint32_t cols, rows, x, y;
    uint32_t fg, bg;
} fb;

static uint32_t pack(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return fb.fmt == FB_BGRX ? (r << 16 | g << 8 | b) : (b << 16 | g << 8 | r);
}

static void clear_rows(uint32_t from_px, uint32_t to_px)
{
    for (uint32_t y = from_px; y < to_px; y++) {
        uint32_t *row = (uint32_t *)(fb.base + (uint64_t)y * fb.pitch);
        for (uint32_t x = 0; x < fb.w; x++) row[x] = fb.bg;
    }
}

static void draw_glyph(uint32_t cx, uint32_t cy, char ch)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    const uint8_t *g = font8x16[ch - FONT_FIRST];
    for (uint32_t y = 0; y < FONT_H; y++) {
        uint32_t *row = (uint32_t *)(fb.base + (uint64_t)(cy * FONT_H + y) * fb.pitch) + cx * FONT_W;
        for (uint32_t x = 0; x < FONT_W; x++) row[x] = (g[y] & (0x80 >> x)) ? fb.fg : fb.bg;
    }
}

static void scroll(void)
{
    uint64_t line = (uint64_t)FONT_H * fb.pitch;
    memmove(fb.base, fb.base + line, line * (fb.rows - 1));
    clear_rows((fb.rows - 1) * FONT_H, fb.rows * FONT_H);
    fb.y = fb.rows - 1;
}

static void putc_(char c)
{
    switch (c) {
    case '\n': fb.x = 0; fb.y++; break;
    case '\r': fb.x = 0; break;
    case '\b': if (fb.x) fb.x--; break;
    case '\t': fb.x = (fb.x + 8) & ~7u; break;
    case '\f': clear_rows(0, fb.h); fb.x = fb.y = 0; return;
    default:
        if (fb.x >= fb.cols) { fb.x = 0; fb.y++; }
        if (fb.y >= fb.rows) scroll();
        draw_glyph(fb.x, fb.y, c);
        fb.x++;
        return;
    }
    if (fb.x >= fb.cols) { fb.x = 0; fb.y++; }
    if (fb.y >= fb.rows) scroll();
}

static void fbcon_write(struct console *c, const char *s, size_t n)
{
    (void)c;
    for (size_t i = 0; i < n; i++) putc_(s[i]);
}

static struct console fbcon_console = {.name = "fbcon", .write = fbcon_write};

static int fbcon_probe(const struct boot_info *bi)
{
    if (!bi->fb.base || bi->fb.format == FB_NONE) return -1;
    fb.base = (uint8_t *)bi->fb.base;
    fb.w = bi->fb.width; fb.h = bi->fb.height; fb.pitch = bi->fb.pitch; fb.fmt = bi->fb.format;
    fb.cols = fb.w / FONT_W; fb.rows = fb.h / FONT_H;
    fb.fg = pack(FG); fb.bg = pack(BG);
    clear_rows(0, fb.h);
    console_register(&fbcon_console);
    return 0;
}

MIGA_DRIVER(fbcon, "fbcon", DRV_VER(1, 0), DRV_EARLY, fbcon_probe);

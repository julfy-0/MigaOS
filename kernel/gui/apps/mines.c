/* Minesweeper 15x10, 24 мины. Мышь: ЛКМ открыть, ПКМ флаг. Клавиши: стрелки, Space/Enter, F, R. */
#include <kernel/gui.h>
#include <miga/time.h>

#define GW 15
#define GH 10
#define CS 26
#define OX 10
#define OY 50
#define NMINES 24

struct cell { uint8_t mine, adj, st; };            /* st: 0 закрыта, 1 открыта, 2 флаг */
static struct cell g[GH][GW];
static int placed, over, won, opened, flags, kx, ky, kbd;
static uint32_t t0, endt, seed;
static const uint32_t numc[9] = {0, 0x4C8DFF, 0x4CD08A, 0xE5566D, 0x8A4FD8, 0xF2B84B, 0x2FBFBF, 0xFFFFFF, 0x8B95AD};

static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static void reset(void)
{
    for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) g[y][x] = (struct cell){0, 0, 0};
    placed = over = won = opened = flags = 0; endt = 0;
    seed = (uint32_t)time_now_us() | 1;
}

static void place(int sx, int sy)
{
    for (int k = 0; k < NMINES;) {
        int x = (int)(rnd() % GW), y = (int)(rnd() % GH);
        if (g[y][x].mine || (x == sx && y == sy)) continue;
        g[y][x].mine = 1; k++;
    }
    for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) {
        int n = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < GW && ny < GH && g[ny][nx].mine) n++;
        }
        g[y][x].adj = (uint8_t)n;
    }
}

static void reveal(int x, int y)
{
    static int q[GW * GH]; int n = 0;
    if (g[y][x].st) return;
    g[y][x].st = 1; opened++; q[n++] = y * GW + x;
    if (g[y][x].mine) { over = 1; return; }
    for (int i = 0; i < n; i++) {
        int cx = q[i] % GW, cy = q[i] / GW;
        if (g[cy][cx].adj) continue;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = cx + dx, ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= GW || ny >= GH || g[ny][nx].st) continue;
            g[ny][nx].st = 1; opened++; q[n++] = ny * GW + nx;
        }
    }
}

static void click(int x, int y, int right)
{
    if (over || won) return;
    struct cell *c = &g[y][x];
    if (right) {
        if (c->st == 0) { c->st = 2; flags++; } else if (c->st == 2) { c->st = 0; flags--; }
        return;
    }
    if (!placed) { place(x, y); placed = 1; t0 = (uint32_t)time_uptime_ms(); }
    reveal(x, y);
    if (!over && opened == GW * GH - NMINES) won = 1;
    if (over || won) endt = ((uint32_t)time_uptime_ms() - t0) / 1000;
}

static void draw(struct gui_win *w)
{
    gui_clear(w, GUI_C_WINBG);
    unsigned t = !placed ? 0 : (over || won) ? endt : ((uint32_t)time_uptime_ms() - t0) / 1000;
    gui_fill(w, OX, 8, w->w - 2 * OX, 34, GUI_C_PANEL);
    gui_printf(w, OX + 8, 17, GUI_C_TEXT, "Mines %d   Time %u", NMINES - flags, t);
    const char *s = over ? "BOOM! press R" : won ? "You win! press R" : "R = restart";
    gui_text(w, w->w - OX - 8 - gui_text_width(s), 17, s, over ? GUI_C_BAD : won ? GUI_C_GOOD : GUI_C_DIM);

    for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) {
        const struct cell *c = &g[y][x];
        int px = OX + x * CS, py = OY + y * CS;
        int show_mine = over && c->mine && c->st != 2;
        gui_fill(w, px, py, CS - 1, CS - 1, show_mine ? GUI_C_BAD : c->st == 1 ? 0x1B2236 : GUI_C_PANEL2);
        gui_rect(w, px, py, CS - 1, CS - 1, GUI_C_LINE);
        if (c->st == 2) gui_text(w, px + 9, py + 5, "F", GUI_C_WARN);
        else if (show_mine) gui_text(w, px + 9, py + 5, "*", 0xFFFFFF);
        else if (c->st == 1 && c->adj) { char b[2] = {(char)('0' + c->adj), 0}; gui_text(w, px + 9, py + 5, b, numc[c->adj]); }
    }
    if (kbd) gui_rect(w, OX + kx * CS - 1, OY + ky * CS - 1, CS + 1, CS + 1, GUI_C_WARN);
    gui_present(w);
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type == GUI_EV_TICK) { if (placed && !over && !won) draw(w); return; }
    if (e->type == GUI_EV_MOUSE && e->mtype == GUI_MOUSE_DOWN) {
        int x = e->mx - OX, y = e->my - OY;
        if (x < 0 || y < 0) return;
        x /= CS; y /= CS;
        if (x >= GW || y >= GH) return;
        kbd = 0; click(x, y, e->key == 2); draw(w); return;
    }
    if (e->type != GUI_EV_KEY) return;
    int k = e->key;
    if (k == 'r' || k == 'R') reset();
    else if (k == GUI_KEY_LEFT && kx > 0) kx--;
    else if (k == GUI_KEY_RIGHT && kx < GW - 1) kx++;
    else if (k == GUI_KEY_UP && ky > 0) ky--;
    else if (k == GUI_KEY_DOWN && ky < GH - 1) ky++;
    else if (k == ' ' || k == '\n') click(kx, ky, 0);
    else if (k == 'f' || k == 'F') click(kx, ky, 1);
    else return;
    kbd = 1; draw(w);
}

static void start(struct gui_win *w) { reset(); kx = ky = kbd = 0; draw(w); }

const struct gui_app app_mines = {
    .name = "Minesweeper", .glyph = "Ms", .color = 0xE5566D, .w = 410, .h = 320, .tick_ms = 500,
    .start = start, .event = ev,
};

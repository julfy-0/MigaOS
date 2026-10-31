/* Приложение 5: Snake — игра. Стрелки или WASD, пробел — пауза, R — заново. Ускоряется по мере роста счёта. */
#include <kernel/gui.h>
#include <miga/time.h>
#include <miga/string.h>

#define GW 20
#define GH 20
#define CELL 14
#define BX 10
#define BY 40

static uint8_t sx[GW * GH], sy[GW * GH];
static unsigned len, score, best;
static int dir, ndir;                  /* 0 вверх, 1 вправо, 2 вниз, 3 влево */
static int fx, fy, alive, paused, started;
static uint32_t rng = 2463534242u;

static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int occupied(int x, int y)
{
    for (unsigned i = 0; i < len; i++) if (sx[i] == x && sy[i] == y) return 1;
    return 0;
}

static void place_food(void)
{
    for (int tries = 0; tries < 400; tries++) {
        fx = (int)(rnd() % GW); fy = (int)(rnd() % GH);
        if (!occupied(fx, fy)) return;
    }
    for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) if (!occupied(x, y)) { fx = x; fy = y; return; }
}

static unsigned speed_ms(void)
{
    unsigned s = 130 - (score / 4) * 8;
    return s < 55 ? 55 : s;
}

static void reset(struct gui_win *w)
{
    len = 3; score = 0; dir = ndir = 1; alive = 1; paused = 1; started = 0;
    for (unsigned i = 0; i < len; i++) { sx[i] = (uint8_t)(GW / 2 - (int)i); sy[i] = GH / 2; }
    rng ^= (uint32_t)time_ticks() * 2654435761u;
    if (!rng) rng = 1;
    place_food();
    gui_set_tick(w, speed_ms());
}

static void step(struct gui_win *w)
{
    static const int ddx[4] = {0, 1, 0, -1}, ddy[4] = {-1, 0, 1, 0};
    dir = ndir;
    int nx = sx[0] + ddx[dir], ny = sy[0] + ddy[dir];
    if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) { alive = 0; return; }
    int eat = nx == fx && ny == fy;
    unsigned check = eat ? len : len - 1;                      /* хвост освободится, если не едим */
    for (unsigned i = 0; i < check; i++) if (sx[i] == nx && sy[i] == ny) { alive = 0; return; }
    if (eat && len < GW * GH) len++;
    for (unsigned i = len - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
    sx[0] = (uint8_t)nx; sy[0] = (uint8_t)ny;
    if (eat) {
        score++;
        if (score > best) best = score;
        place_food();
        gui_set_tick(w, speed_ms());
    }
}

static void draw(struct gui_win *w)
{
    gui_clear(w, GUI_C_WINBG);
    gui_printf(w, 10, 10, GUI_C_TEXT, "Score %u", score);
    gui_printf(w, 120, 10, GUI_C_DIM, "Best %u", best);
    gui_printf(w, 220, 10, GUI_C_DIM, "%u ms", speed_ms());

    gui_fill(w, BX, BY, GW * CELL, GH * CELL, GUI_C_PANEL);
    gui_rect(w, BX - 1, BY - 1, GW * CELL + 2, GH * CELL + 2, GUI_C_LINE);
    gui_fill(w, BX + fx * CELL + 2, BY + fy * CELL + 2, CELL - 4, CELL - 4, GUI_C_BAD);
    for (unsigned i = 0; i < len; i++) {
        uint32_t c = i == 0 ? 0x7CF0B0 : (i & 1 ? GUI_C_GOOD : 0x3DB878);
        gui_fill(w, BX + sx[i] * CELL + 1, BY + sy[i] * CELL + 1, CELL - 2, CELL - 2, c);
    }
    const char *msg = 0;
    if (!alive) msg = "GAME OVER - press R";
    else if (!started) msg = "SPACE to start";
    else if (paused) msg = "PAUSED";
    if (msg) {
        int mw = gui_text_width(msg) + 20, mx = BX + (GW * CELL - mw) / 2, my = BY + GH * CELL / 2 - 14;
        gui_fill(w, mx, my, mw, 28, 0x0A0E1A);
        gui_rect(w, mx, my, mw, 28, GUI_C_ACCENT);
        gui_text(w, mx + 10, my + 6, msg, 0xFFFFFF);
    }
    gui_text(w, 10, w->h - 22, "Arrows/WASD move  Space pause  R restart", GUI_C_DIM);
    gui_present(w);
}

static void turn(int d)
{
    if (((d + 2) & 3) != dir) ndir = d;                        /* разворот на 180° запрещён */
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    switch (e->type) {
    case GUI_EV_TICK:
        if (alive && !paused) { step(w); draw(w); }
        break;
    case GUI_EV_BLUR:
        if (started && alive) { paused = 1; draw(w); }
        break;
    case GUI_EV_KEY:
        switch (e->key) {
        case GUI_KEY_UP:    case 'w': case 'W': turn(0); break;
        case GUI_KEY_RIGHT: case 'd': case 'D': turn(1); break;
        case GUI_KEY_DOWN:  case 's': case 'S': turn(2); break;
        case GUI_KEY_LEFT:  case 'a': case 'A': turn(3); break;
        case ' ': if (alive) { paused = !paused; started = 1; } break;
        case 'r': case 'R': reset(w); break;
        default: return;
        }
        if (e->key != ' ' && e->key != 'r' && e->key != 'R' && alive) { paused = 0; started = 1; }
        draw(w);
        break;
    }
}

static void start(struct gui_win *w) { reset(w); draw(w); }

const struct gui_app app_snake = {
    .name = "Snake", .glyph = "Sn", .color = 0x3DB878, .w = 300, .h = 350, .tick_ms = 130,
    .start = start, .event = ev,
};

/* Форма курсора мыши: классическая стрелка Windows (белая заливка, чёрный контур, 12x19, горячая точка в левом
 * верхнем углу). Рисует её server.c прямо во фреймбуфер поверх backbuffer'а, поэтому движение мыши не требует
 * перерисовки кадра. */
#include "gui_priv.h"

static const char *const art[CUR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      X..X  ",
    "       XX   ",
};

static uint8_t shape[CUR_W * CUR_H];
static int built;

const uint8_t *cursor_shape(void)
{
    if (!built) {
        for (int y = 0; y < CUR_H; y++)
            for (int x = 0; x < CUR_W; x++) {
                char c = art[y][x];
                shape[y * CUR_W + x] = c == 'X' ? 1 : c == '.' ? 2 : 0;
            }
        built = 1;
    }
    return shape;
}

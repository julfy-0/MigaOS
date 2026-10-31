/* Драйвер PS/2-мыши (второй порт контроллера i8042, IRQ12).
 * ps2kbd при инициализации отключает второй порт; здесь (стадия LATE) его включаем, сбрасываем мышь,
 * пробуем включить колесо (протокол IntelliMouse, ID 3) и 5 кнопок (ID 4), ставим 200 отчётов/с
 * и принимаем 3/4-байтовые пакеты. Каждый пакет уходит в mouse_push(). */
#define MIGA_TAG "ps2mouse"
#include <miga/driver.h>
#include <miga/irq.h>
#include <miga/input.h>
#include <miga/io.h>
#include <miga/log.h>

#define DATA 0x60
#define CMD  0x64

static int wait_write(void) { for (int i = 0; i < 100000; i++) { if (!(inb(CMD) & 2)) return 0; io_wait(); } return -1; }
static int wait_read(void)  { for (int i = 0; i < 300000; i++) { if (inb(CMD) & 1) return 0; io_wait(); } return -1; }
static void flush(void)     { for (int i = 0; i < 64 && (inb(CMD) & 1); i++) { inb(DATA); io_wait(); } }

static int ctl(uint8_t c) { if (wait_write()) return -1; outb(CMD, c); return 0; }

/* байт мыши с ожиданием; -1 при таймауте */
static int rd(void) { return wait_read() ? -1 : inb(DATA); }

/* команда мыши (через 0xD4) + ACK 0xFA */
static int mcmd(uint8_t b)
{
    for (int tries = 0; tries < 3; tries++) {
        if (ctl(0xD4) || wait_write()) return -1;
        outb(DATA, b);
        int r = rd();
        if (r == 0xFA) return 0;
        if (r != 0xFE) return -1;
    }
    return -1;
}

static int mcmd_arg(uint8_t b, uint8_t a) { return mcmd(b) || mcmd(a); }

static int get_id(void)
{
    if (mcmd(0xF2)) return -1;
    return rd();
}

static int mouse_id;
static uint8_t pkt[4];
static unsigned npkt, plen = 3;

static void ps2m_irq(void *ctx)
{
    (void)ctx;
    for (int guard = 0; guard < 16; guard++) {
        uint8_t st = inb(CMD);
        if (!(st & 1) || !(st & 0x20)) break;            /* пусто или байт клавиатуры: он не наш */
        uint8_t b = inb(DATA);

        if (npkt == 0 && !(b & 8)) continue;             /* рассинхрон: первый байт всегда с битом 3 */
        pkt[npkt++] = b;
        if (npkt < plen) continue;
        npkt = 0;

        uint8_t f = pkt[0];
        if (f & 0xC0) continue;                          /* переполнение счётчиков: пакет мусорный */
        int dx = pkt[1] - ((f << 4) & 0x100);
        int dy = pkt[2] - ((f << 3) & 0x100);
        int wheel = 0;
        unsigned btn = f & 7;
        if (plen == 4) {
            int z = pkt[3] & 0x0F;
            if (z & 8) z -= 16;
            wheel = -z;                                  /* PS/2: положительный Z = колесо к себе */
            if (mouse_id == 4) { /* кнопки 4/5 пока не используем */ }
        }
        mouse_push(dx, -dy, wheel, btn);                 /* PS/2: Y вверх положителен, экран — вниз */
    }
}

static int ps2m_probe(const struct boot_info *bi)
{
    (void)bi;
    if (inb(CMD) == 0xFF) return -1;                     /* контроллера нет */
    flush();

    if (ctl(0xA8)) return -1;                            /* включить второй порт */
    if (ctl(0x20) || wait_read()) return -1;
    uint8_t cfg = inb(DATA);
    cfg |= 0x02;                                         /* IRQ12 */
    cfg &= (uint8_t)~0x20;                               /* тактирование порта 2 */
    if (ctl(0x60) || wait_write()) return -1;
    outb(DATA, cfg);
    flush();

    if (mcmd(0xFF)) return -2;                           /* reset: нет ACK -> мыши нет */
    int r = rd();                                        /* self-test: 0xAA, затем ID 0x00 */
    if (r != 0xAA) return -3;
    rd();
    flush();

    mcmd(0xF6);                                          /* defaults */
    /* IntelliMouse: magic sample-rate 200,100,80 -> ID 3 (колесо); затем 200,200,80 -> ID 4 (5 кнопок) */
    mcmd_arg(0xF3, 200); mcmd_arg(0xF3, 100); mcmd_arg(0xF3, 80);
    mouse_id = get_id();
    if (mouse_id == 3) {
        mcmd_arg(0xF3, 200); mcmd_arg(0xF3, 200); mcmd_arg(0xF3, 80);
        int id2 = get_id();
        if (id2 == 4) mouse_id = 4;
    }
    plen = (mouse_id == 3 || mouse_id == 4) ? 4 : 3;
    if (mouse_id < 0) { mouse_id = 0; plen = 3; }

    mcmd_arg(0xE8, 3);                                   /* разрешение 8 отсчётов/мм */
    mcmd_arg(0xF3, 200);                                 /* 200 отчётов/с (максимум стандарта PS/2) */
    if (mcmd(0xF4)) return -4;                           /* включить поток данных */
    flush();
    npkt = 0;

    int rc = irq_register(12, ps2m_irq, 0);
    if (rc) return rc;
    mouse_present("ps2");
    pr_info("PS/2 mouse, id %d%s, 200 Hz\n", mouse_id, plen == 4 ? " (wheel)" : "");
    return 0;
}

MIGA_DRIVER(ps2mouse, "ps2mouse", DRV_VER(1, 0), DRV_LATE, ps2m_probe);

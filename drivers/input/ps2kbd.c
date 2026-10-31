/* Драйвер PS/2-клавиатуры: инициализация контроллера i8042, scancode set 1 (через трансляцию),
 * раскладка US, Shift/Caps/Ctrl, IRQ1. Если контроллера или клавиатуры нет — probe() вернёт ошибку. */
#define MIGA_TAG "kbd"
#include <miga/driver.h>
#include <miga/irq.h>
#include <miga/input.h>
#include <miga/io.h>
#include <miga/log.h>

#define KBD_DATA 0x60
#define KBD_CMD  0x64

static const char plain[0x3A] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b', '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']', '\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
};
static const char shifted[0x3A] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b', '\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ',
};

static int shift, caps, ctrl, ext;

/* ---- i8042 ---- */
static int wait_write(void) { for (int i = 0; i < 100000; i++) { if (!(inb(KBD_CMD) & 2)) return 0; io_wait(); } return -1; }
static int wait_read(void)  { for (int i = 0; i < 600000; i++) { if (inb(KBD_CMD) & 1) return 0; io_wait(); } return -1; }
static void flush(void)     { for (int i = 0; i < 64 && (inb(KBD_CMD) & 1); i++) { inb(KBD_DATA); io_wait(); } }

static int ctl_cmd(uint8_t c)  { if (wait_write()) return -1; outb(KBD_CMD, c); return 0; }
static int dev_send(uint8_t b) { if (wait_write()) return -1; outb(KBD_DATA, b); return 0; }
static int dev_read(void)      { return wait_read() ? -1 : inb(KBD_DATA); }

static int dev_cmd(uint8_t b)  /* команда устройству + ожидание ACK (0xFA) */
{
    for (int tries = 0; tries < 3; tries++) {
        if (dev_send(b)) return -1;
        int r = dev_read();
        if (r == 0xFA) return 0;
        if (r != 0xFE) return -1; /* 0xFE = resend */
    }
    return -1;
}

static int super_down;                          /* клавиша Win сейчас зажата */

static void kbd_irq(void *ctx)
{
    (void)ctx;
    uint8_t sc = inb(KBD_DATA);

    if (sc == 0xE0) { ext = 1; return; }
    int release = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (ext) {                                  /* E0-коды: правый Ctrl, Enter и '/' на keypad, стрелки, Win, навигация */
        ext = 0;
        if (code == 0x1D) { ctrl = !release; return; }
        if (code == 0x5B || code == 0x5C) {     /* левый/правый Win: пушим только переход «отпущена -> нажата» (без typematic) */
            if (release) super_down = 0;
            else if (!super_down) { super_down = 1; input_push((char)0x84); }   /* GUI_KEY_SUPER */
            return;
        }
        if (release) return;
        switch (code) {
        case 0x1C: input_push('\n'); break;
        case 0x35: input_push('/'); break;
        case 0x48: input_push((char)0x80); break;   /* стрелки -> GUI_KEY_UP/DOWN/LEFT/RIGHT */
        case 0x50: input_push((char)0x81); break;
        case 0x4B: input_push((char)0x82); break;
        case 0x4D: input_push((char)0x83); break;
        case 0x53: input_push((char)0x85); break;   /* Delete */
        case 0x47: input_push((char)0x86); break;   /* Home */
        case 0x4F: input_push((char)0x87); break;   /* End */
        case 0x49: input_push((char)0x88); break;   /* Page Up */
        case 0x51: input_push((char)0x89); break;   /* Page Down */
        }
        return;
    }

    if (code == 0x2A || code == 0x36) { shift = !release; return; }
    if (code == 0x1D) { ctrl = !release; return; }
    if (release) return;
    if (code == 0x3A) { caps = !caps; return; }
    if (code >= sizeof plain) return;

    char c = shift ? shifted[code] : plain[code];
    if (caps && c >= 'a' && c <= 'z') c -= 32;
    else if (caps && shift && c >= 'A' && c <= 'Z') c += 32;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1F;
    if (c) input_push(c);
}

static int kbd_probe(const struct boot_info *bi)
{
    (void)bi;
    if (inb(KBD_CMD) == 0xFF) return -1;       /* шина «плавает»: контроллера i8042 нет */

    flush();
    if (ctl_cmd(0xAD) || ctl_cmd(0xA7)) return -1; /* отключить оба порта на время настройки */
    flush();

    if (ctl_cmd(0xAA) || wait_read() || inb(KBD_DATA) != 0x55)
        pr_warn("i8042 self-test failed, continuing\n");

    if (ctl_cmd(0x20) || wait_read()) return -1;   /* читаем command byte */
    uint8_t cfg = inb(KBD_DATA);
    cfg |= 0x01 | 0x40;                            /* IRQ1 + трансляция set2 -> set1 */
    cfg &= ~0x02;                                  /* IRQ12 не нужен */
    if (ctl_cmd(0x60) || wait_write()) return -1;
    outb(KBD_DATA, cfg);

    if (ctl_cmd(0xAE)) return -1;                  /* включить порт 1 */
    flush();

    if (dev_cmd(0xFF)) return -2;                  /* reset клавиатуры: нет ACK -> клавиатуры нет */
    if (dev_read() != 0xAA) return -3;             /* self-test устройства */
    flush();
    if (dev_cmd(0xF4)) return -4;                  /* включить сканирование */

    return irq_register(1, kbd_irq, 0);
}

MIGA_DRIVER(ps2kbd, "ps2kbd", DRV_VER(2, 0), DRV_NORMAL, kbd_probe);

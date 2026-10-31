/* Драйвер UART 16550 (COM1). Раннее логирование: удобно ловить через `qemu -serial stdio`. */
#define MIGA_TAG "uart"
#include <miga/driver.h>
#include <miga/console.h>
#include <miga/io.h>
#include <miga/log.h>

#define COM1 0x3F8

static void uart_putc(char c)
{
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++) ;
    outb(COM1, (uint8_t)c);
}

static void uart_write(struct console *c, const char *s, size_t n)
{
    (void)c;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\f') continue;
        if (s[i] == '\n') uart_putc('\r');
        uart_putc(s[i]);
    }
}

static struct console uart_console = {.name = "uart0", .write = uart_write};

static int uart_probe(const struct boot_info *bi)
{
    (void)bi;
    outb(COM1 + 7, 0xAE);                 /* scratch-регистр: есть ли вообще UART? */
    if (inb(COM1 + 7) != 0xAE) return -1;

    outb(COM1 + 1, 0x00);                 /* без прерываний */
    outb(COM1 + 3, 0x80);                 /* DLAB */
    outb(COM1 + 0, 0x01);                 /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);                 /* 8N1 */
    outb(COM1 + 2, 0xC7);                 /* FIFO */
    outb(COM1 + 4, 0x0B);                 /* DTR, RTS, OUT2 */
    console_register(&uart_console);
    return 0;
}

MIGA_DRIVER(uart, "uart16550", DRV_VER(1, 0), DRV_EARLY, uart_probe);

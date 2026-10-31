/* Драйвер таймера 8254 PIT: канал 0, 1000 Гц, источник системных тиков.
 * 1 кГц нужно GUI: на 240 Гц кадр = 4,17 мс, а опрос USB-мыши идёт на каждом пробуждении цикла. */
#define MIGA_TAG "pit"
#include <miga/driver.h>
#include <miga/irq.h>
#include <miga/time.h>
#include <miga/io.h>
#include <miga/log.h>

#define PIT_HZ 1000
#define PIT_BASE_HZ 1193182

static void pit_irq(void *ctx) { (void)ctx; time_tick(); }

static int pit_probe(const struct boot_info *bi)
{
    (void)bi;
    uint16_t div = PIT_BASE_HZ / PIT_HZ;
    outb(0x43, 0x34);                          /* канал 0, lo/hi, режим 2 (rate generator) */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    time_set_hz(PIT_HZ);
    return irq_register(0, pit_irq, 0);
}

MIGA_DRIVER(pit, "pit8254", DRV_VER(1, 0), DRV_NORMAL, pit_probe);

#define MIGA_TAG "time"
#include <miga/time.h>
#include <miga/io.h>
#include <kernel/kernel.h>

static volatile uint64_t ticks;
static uint32_t hz = 100;
static uint64_t tsc_khz;
static uint64_t tsc_base;

void time_set_hz(uint32_t h) { hz = h ? h : 100; }
void time_tick(void) { ticks++; }
uint64_t time_ticks(void) { return ticks; }
uint64_t time_uptime_ms(void) { return ticks * 1000 / hz; }

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint32_t time_tsc_khz(void) { return (uint32_t)tsc_khz; }

uint64_t time_now_us(void)
{
    if (!tsc_khz) return time_uptime_ms() * 1000;
    uint64_t d = rdtsc() - tsc_base;
    /* d * 1000 / khz без переполнения: делим на целую и дробную часть */
    return (d / tsc_khz) * 1000 + (d % tsc_khz) * 1000 / tsc_khz;
}

/* Калибровка TSC по каналу 2 PIT (одноразовый режим 0, ~20 мс). Канал 0 (системные тики) не трогаем.
 * Измеряем 3 раза и берём минимальную длительность (самая «чистая» из-за возможных SMI/эмуляции). */
void time_init(void)
{
    uint64_t best = 0;
    for (int k = 0; k < 3; k++) {
        uint16_t count = 23864;                                   /* 1193182 Гц * 20 мс */
        outb(0x61, (uint8_t)((inb(0x61) & ~0x02) | 0x01));         /* gate2 = 1, динамик выключен */
        outb(0x43, 0xB0);                                          /* ch2, lo/hi, режим 0, бинарный */
        outb(0x42, count & 0xFF);
        outb(0x42, count >> 8);
        outb(0x61, (uint8_t)(inb(0x61) & ~0x01));                  /* gate low -> high: старт отсчёта */
        outb(0x61, (uint8_t)(inb(0x61) | 0x01));
        uint64_t t0 = rdtsc();
        unsigned spin = 0;
        while (!(inb(0x61) & 0x20) && ++spin < 50000000u) {}       /* OUT2 = 1: досчитал */
        uint64_t t1 = rdtsc();
        if (spin >= 50000000u) continue;
        uint64_t d = t1 - t0;
        if (!best || d < best) best = d;
    }
    outb(0x61, (uint8_t)(inb(0x61) & ~0x03));
    if (!best) { pr_warn("time: TSC calibration failed, using timer ticks\n"); return; }
    /* 20 мс = best тактов  =>  khz = best / 20 */
    tsc_khz = best * 1000 / 20000;
    if (tsc_khz < 100000 || tsc_khz > 10000000) { pr_warn("time: TSC %lu kHz out of range, ignored\n", tsc_khz); tsc_khz = 0; return; }
    tsc_base = rdtsc();
    pr_info("time: TSC %lu.%03lu MHz\n", tsc_khz / 1000, tsc_khz % 1000);
}

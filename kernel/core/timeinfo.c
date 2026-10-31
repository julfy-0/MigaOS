/* Время: RTC (CMOS), HPET (по адресу из ACPI), TSC, аптайм. */
#include <kernel/kernel.h>
#include <kernel/acpi.h>
#include <kernel/sysinfo.h>
#include <miga/io.h>
#include <miga/mem.h>
#include <miga/time.h>

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); io_wait(); return inb(0x71); }

static int rtc_read(unsigned v[6])
{
    unsigned prev[6] = {0};
    for (int tries = 0; tries < 4; tries++) {
        for (unsigned spin = 0; (cmos(0x0A) & 0x80) && spin < 100000; spin++) io_wait();   /* update in progress */
        unsigned cur[6] = {cmos(0x00), cmos(0x02), cmos(0x04), cmos(0x07), cmos(0x08), cmos(0x09)};
        uint8_t b = cmos(0x0B);
        int same = 1;
        for (int i = 0; i < 6; i++) if (cur[i] != prev[i]) same = 0;
        for (int i = 0; i < 6; i++) prev[i] = cur[i];
        if (!same) continue;
        int bcd = !(b & 4), pm = cur[2] & 0x80;
        for (int i = 0; i < 6; i++) {
            unsigned x = cur[i] & (i == 2 ? 0x7F : 0xFF);
            v[i] = bcd ? (x & 0xF) + (x >> 4) * 10 : x;
        }
        if (!(b & 2) && pm) v[2] = (v[2] + 12) % 24;                      /* 12-часовой режим */
        v[5] += 2000;
        return 0;
    }
    return -1;
}

void timeinfo_print(void)
{
    kprintf("\n:: time\n");
    unsigned t[6];
    if (!rtc_read(t))
        kprintf("  RTC: %04u-%02u-%02u %02u:%02u:%02u (hardware clock; local time or UTC depending on OS)\n", t[5], t[4], t[3], t[2], t[1], t[0]);
    else kprintf("  RTC: unstable reading\n");

    uint64_t h = acpi_hpet_base();
    if (h) {
        volatile uint32_t *r = mmio_map(h, 0x400);
        if (r) {
            uint32_t cap = r[0], period = r[1];                            /* период в фемтосекундах */
            if (cap != 0xFFFFFFFFu && period)
                kprintf("  HPET at %#lx: rev %u, %u timer(s), %s counter, %u kHz\n", h, cap & 0xFF, ((cap >> 8) & 0x1F) + 1,
                        (cap >> 13 & 1) ? "64-bit" : "32-bit", (unsigned)(1000000000000ULL / period));
            else kprintf("  HPET at %#lx: not responding\n", h);
        }
    } else kprintf("  HPET: not present\n");

    uint64_t ms = time_uptime_ms();
    kprintf("  uptime %lu.%03lu s (kernel timer ticks %lu)\n", ms / 1000, ms % 1000, time_ticks());
}

/* Часы реального времени (CMOS RTC) для GUI: дата и время с учётом часового пояса из реестра настроек (sys.tz). */
#include <kernel/gui.h>
#include <kernel/settings.h>
#include <miga/io.h>

static uint8_t cmos(uint8_t r) { outb(0x70, r); io_wait(); return inb(0x71); }

/* Алгоритмы Говарда Хиннанта: дни от 1970-01-01 <-> гражданская дата. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

int gui_days_in_month(int y, int m)
{
    static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : dm[m - 1];
}

int gui_weekday(int y, int m, int d) { return (int)((days_from_civil(y, m, d) % 7 + 11) % 7); }   /* 0 = воскресенье */

void gui_get_time(struct gui_time *t)
{
    uint8_t v[6], w[6];
    int tries = 0;
    do {                                            /* читаем дважды, пока значения не совпадут */
        for (int spin = 0; (cmos(0x0A) & 0x80) && spin < 100000; spin++) io_wait();
        v[0] = cmos(0); v[1] = cmos(2); v[2] = cmos(4); v[3] = cmos(7); v[4] = cmos(8); v[5] = cmos(9);
        for (int spin = 0; (cmos(0x0A) & 0x80) && spin < 100000; spin++) io_wait();
        w[0] = cmos(0); w[1] = cmos(2); w[2] = cmos(4); w[3] = cmos(7); w[4] = cmos(8); w[5] = cmos(9);
    } while (++tries < 3 && (v[0] != w[0] || v[1] != w[1] || v[2] != w[2] || v[3] != w[3] || v[4] != w[4] || v[5] != w[5]));
    uint8_t b = cmos(0x0B);
    int pm = v[2] & 0x80;
    v[2] &= 0x7F;
    if (!(b & 4)) for (int i = 0; i < 6; i++) v[i] = (uint8_t)((v[i] & 15) + (v[i] >> 4) * 10);
    if (!(b & 2)) v[2] = (uint8_t)(v[2] % 12 + (pm ? 12 : 0));
    int64_t secs = days_from_civil(2000 + v[5], v[4] ? v[4] : 1, v[3] ? v[3] : 1) * 86400 + v[2] * 3600 + v[1] * 60 + v[0]
                 + (int64_t)settings_get_int("sys.tz", 0) * 3600;
    int64_t days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    int rem = (int)(secs - days * 86400);
    civil_from_days(days, &t->year, &t->mon, &t->day);
    t->hour = rem / 3600; t->min = rem % 3600 / 60; t->sec = rem % 60;
    t->wday = (int)((days % 7 + 11) % 7);
}

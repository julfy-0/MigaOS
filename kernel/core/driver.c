/* Реестр драйверов: список строится линкером в секции .miga_drivers.
 * После probe() печатается строка вида:
 *   ps2kbd ............... v01.00 [ OK ]
 * DRV_EARLY: консоли появляются только после probe() uart/fbcon, поэтому сначала
 * зондируем все драйверы стадии, а отчёт печатаем после. */
#include <kernel/kernel.h>
#include <miga/string.h>

extern const struct miga_driver __drivers_start[], __drivers_end[];

#define MAX_DRV 128
#define NAME_COL 24

static const char *const stage_name[] = {"early", "normal", "late"};
static int result[MAX_DRV];

static void report(const struct miga_driver *d, int rc)
{
    char dots[NAME_COL + 1];
    size_t n = strlen(d->name), k = 0;
    if (n < NAME_COL) for (; k < NAME_COL - n; k++) dots[k] = '.';
    dots[k] = 0;
    kprintf("  %s %s v%02u.%02u [%s]", d->name, dots,
            (unsigned)(d->version >> 8), (unsigned)(d->version & 0xFF), rc == 0 ? " OK " : "FAIL");
    if (rc) kprintf(" (code %d)", rc);
    kprintf("\n");
}

void drivers_init(const struct boot_info *bi, int stage)
{
    int deferred = (stage == DRV_EARLY);

    if (!deferred) kprintf("\n:: drivers (%s)\n", stage_name[stage]);

    for (const struct miga_driver *d = __drivers_start; d < __drivers_end; d++) {
        if (d->stage != stage) continue;
        int rc = d->probe(bi);
        size_t i = (size_t)(d - __drivers_start);
        if (i < MAX_DRV) result[i] = rc;
        if (!deferred) report(d, rc);
    }

    if (deferred) {
        kprintf("\n:: drivers (%s)\n", stage_name[stage]);
        for (const struct miga_driver *d = __drivers_start; d < __drivers_end; d++) {
            size_t i = (size_t)(d - __drivers_start);
            if (d->stage == stage) report(d, i < MAX_DRV ? result[i] : 0);
        }
    }
}

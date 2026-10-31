/* Сведения о процессоре и оперативной памяти: CPUID, ACPI MADT, карта памяти UEFI, SMBIOS. */
#pragma once
#include <miga/boot_info.h>

/* Собирает данные в статические структуры. Вызывать после paging_init и ДО pmm_init:
 * таблицы SMBIOS/ACPI могут лежать в памяти, которую PMM отдаёт под свои нужды. */
void sysinfo_init(const struct boot_info *bi);
void sysinfo_cpu_print(void);
void sysinfo_ram_print(void);
unsigned sysinfo_tsc_mhz(void);
void sysinfo_board_print(void);   /* BIOS, система, плата, шасси из SMBIOS (типы 0-3) */

struct cpu_live {
    unsigned tsc_mhz;     /* частота TSC */
    unsigned eff_mhz;     /* средняя реальная частота по APERF/MPERF, 0 = недоступно (виртуалка/нет MSR) */
    int temp_c;           /* температура ядра, -1000 = недоступно (только Intel с DTS, не в виртуалке) */
    unsigned tjmax;
    unsigned throttled;
};
int sysinfo_cpu_live(struct cpu_live *out);   /* ~100 мс, нужны прерывания; 0 = успех */
void timeinfo_print(void);                    /* RTC, HPET, TSC, аптайм (kernel/core/timeinfo.c) */   /* измеряет TSC по таймеру ядра ~100 мс; нужны включённые прерывания; 0 = не вышло */

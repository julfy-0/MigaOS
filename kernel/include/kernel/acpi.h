/* ACPI: список таблиц, FADT/MADT/MCFG/HPET и признаки питания (поиск HID в AML, без интерпретатора). */
#pragma once
#include <miga/boot_info.h>

void     acpi_init(const struct boot_info *bi);   /* после paging_init, до pmm_init */
void     acpi_print(void);
uint64_t acpi_hpet_base(void);                    /* 0 = HPET нет */

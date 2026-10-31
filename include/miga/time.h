#pragma once
#include <stdint.h>

void time_set_hz(uint32_t hz); /* вызывает драйвер таймера при настройке */
void time_tick(void);          /* вызывает драйвер таймера из своего IRQ */
uint64_t time_ticks(void);
uint64_t time_uptime_ms(void);

/* Микросекундные часы на TSC (калибруются по PIT в time_init()). Нужны для кадров 240 Гц:
 * период кадра 4167 мкс, а тик PIT = 1 мс. Если TSC откалибровать не удалось — считают по тикам. */
uint64_t time_now_us(void);
uint32_t time_tsc_khz(void);   /* 0 = TSC не откалиброван */

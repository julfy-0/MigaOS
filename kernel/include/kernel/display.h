/* Монитор: разбор EDID (его передаёт загрузчик) + текущий режим фреймбуфера. */
#pragma once
#include <miga/boot_info.h>

void display_init(const struct boot_info *bi);   /* копирует и разбирает EDID; вызывать до pmm_init */
void display_print(void);

/* Максимальная частота обновления, которую заявил монитор (по всем таймингам EDID: базовый блок, CTA-861,
 * DisplayID, и по Range Limits). 0 = EDID нет или частоту определить не удалось. Если пользователь задал `hz N`,
 * возвращается N. ОС не может сама переключить частоту монитора (нет драйвера модесета GPU): режим выбирает
 * прошивка (GOP), эта величина задаёт темп кадров композитора. */
unsigned display_refresh_hz(void);
void     display_set_refresh_hz(unsigned hz);    /* 0 = снова брать из EDID */
unsigned display_edid_native(unsigned *w, unsigned *h); /* родное разрешение из EDID (первый DTD); 0 = неизвестно */
void     display_set_edid(const void *edid, unsigned len); /* EDID от драйвера GPU (DDC), если прошивка не дала свой */

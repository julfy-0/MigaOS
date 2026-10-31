/* Публичный интерфейс драйвера Intel iGPU/Arc (drivers/video/gpu/intel*.c).
 * Драйвер читает состояние display engine, EDID по DDC (GMBUS/AUX), считает частоту кадров по счётчику vblank. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define IGFX_MAX_HEADS 4

struct igfx_head {
    uint8_t  pipe, trans;          /* pipe 0..3 = A..D; trans 0..3 = A..D, 4 = EDP */
    uint8_t  enabled, plane_on, tiled;
    uint32_t hactive, vactive, htotal, vtotal;
    uint32_t hz_milli;             /* измеренная частота кадров, Гц*1000 (0 = не измерена) */
    uint32_t stride;               /* байт в строке (linear) */
    uint32_t plane_ctl;
    uint64_t surf_gtt;             /* адрес поверхности в GGTT */
    uint64_t surf_phys;            /* физический адрес (через PTE GGTT), 0 = неизвестно */
};

int         igfx_present(void);                           /* 1 = Intel display engine найден и разобран */
const char *igfx_name(void);
unsigned    igfx_heads(void);
int         igfx_head_get(unsigned i, struct igfx_head *out);
int         igfx_wait_vblank(unsigned head);              /* 0 = дождались, -1 = таймаут (pipe не тикает) */
unsigned    igfx_edid(uint8_t *buf, unsigned max);        /* EDID, прочитанный драйвером; 0 = нет */
void        igfx_dump(int with_edid);                     /* человекочитаемый отчёт в лог (команда `igfx`) */

/* Экспериментально: двойная буферизация. Буфер в обычной ОЗУ: перед flip вызывать igfx_flush() на нарисованное. */
void        igfx_flush(const void *p, size_t n);
int         igfx_flip(unsigned head, uint64_t phys, size_t bytes);   /* 0 = ok; стрaйд/формат остаются прежними */

/* Сервисы ядра, которые вызывает драйвер */
void        display_set_edid(const void *edid, unsigned len);

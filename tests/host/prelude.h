/* Подмена привилегированных операций для сборки ядерных файлов под хост (-include этого файла). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <kernel/arch.h>
#include <miga/io.h>

uint64_t mock_cr3(void);
void     mock_flush(uint64_t cr3);
uint8_t  mock_inb(uint16_t); uint16_t mock_inw(uint16_t); uint32_t mock_inl(uint16_t);
void     mock_out(uint16_t port, uint32_t v, int width);

#define cpu_irq_save()        ((uint64_t)0)
#define cpu_irq_restore(f)    ((void)(f))
#define cpu_read_cr3()        mock_cr3()
#define cpu_write_cr3(v)      mock_flush(v)
#define cpu_idle()            ((void)0)
#define inb(p)                mock_inb(p)
#define inw(p)                mock_inw(p)
#define inl(p)                mock_inl(p)
#define outb(p, v)            mock_out((p), (v), 1)
#define outw(p, v)            mock_out((p), (v), 2)
#define outl(p, v)            mock_out((p), (v), 4)

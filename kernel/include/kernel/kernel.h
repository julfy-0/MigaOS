#pragma once
#define MIGA_VERSION "0.1"
#include <stdint.h>
#include <stddef.h>
#include <miga/boot_info.h>
#include <miga/log.h>
#include <miga/driver.h>

__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt, ...);
void irq_dispatch(unsigned irq);
void drivers_init(const struct boot_info *bi, int stage);
void pci_log(void);                   /* список PCI-устройств в лог */
void pci_log_ex(int verbose);         /* verbose: subsystem, IRQ, capabilities, BAR */
void kshell_run(void);
void kshell_exec(const char *line);   /* выполнить одну команду kshell (для терминала в GUI) */
int  kcmds_exec(const char *line);    /* kernel/core/kcmds.c: доп. команды; 1 = выполнена, 0 = такой команды нет */
void kcmds_help(void);                /* список доп. команд для help */
unsigned kcmds_count(void);           /* имена доп. команд (для Tab-дополнения в терминале) */
const char *kcmds_name(unsigned i);
void time_init(void);                 /* калибровка TSC по PIT ch2; до sti */

/* физическая память */
void     pmm_init(const struct boot_info *bi);
uint64_t pmm_alloc(void);          /* физ. адрес страницы 4 КиБ или 0 */
uint64_t pmm_alloc_contig(uint64_t pages); /* n подряд идущих страниц или 0 */
void     pmm_free(uint64_t pa);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
#define PAGE_SIZE ((uint64_t)4096)

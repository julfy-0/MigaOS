#pragma once
#include <stdarg.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vkprintf(const char *fmt, va_list ap);

#ifndef MIGA_TAG
#define MIGA_TAG "kern"
#endif
#define pr_info(fmt, ...) kprintf("[" MIGA_TAG "] " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) kprintf("[" MIGA_TAG "] warn: " fmt, ##__VA_ARGS__)

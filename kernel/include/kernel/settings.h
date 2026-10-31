/* Реестр настроек: фиксированная таблица в памяти ядра (не файл и не RAM-диск).
 * Значения живут до перезагрузки: сохранять их пока некуда (нет записи на диск). */
#pragma once
#include <stdint.h>

#define SET_VAL_MAX 24

enum setting_type { SET_BOOL = 0, SET_INT, SET_STR };

enum { SET_OK = 0, SET_E_NOKEY = -1, SET_E_BAD = -2 };

struct setting_info {
    const char *key, *help, *def;     /* def — значение по умолчанию в текстовом виде */
    uint8_t     type;                 /* enum setting_type */
    int32_t     min, max;             /* для SET_INT */
    char        value[SET_VAL_MAX];   /* bool хранится как "on"/"off" */
};

unsigned    settings_count(void);
int         settings_info(unsigned i, struct setting_info *out);   /* 0 или -1 */
int         settings_find(const char *key);                        /* индекс или -1 */
const char *settings_get(const char *key);                         /* "" если ключа нет */
int         settings_get_int(const char *key, int dflt);           /* bool: on = 1; str: dflt */
int         settings_set(const char *key, const char *val);        /* SET_OK / SET_E_NOKEY / SET_E_BAD */
int         settings_set_int(const char *key, int v);
int         settings_reset(const char *key);                       /* NULL или "all" = все */
const char *settings_type_name(unsigned type);

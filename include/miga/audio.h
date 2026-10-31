/* Публичный интерфейс аудиоконтроллеров для ядра. Реализация — drivers/audio/hda.c.
 * Intel HD Audio: опознание контроллера и кодеков, без воспроизведения. */
#pragma once
#include <stdint.h>

#define AUDIO_MAX 4
#define AUDIO_CODECS 4

struct audio_codec { uint8_t addr; uint16_t vendor, device; uint32_t rev; };

struct audio_info {
    uint8_t  pci_bus, pci_dev, pci_fn;
    uint16_t vendor, device;
    uint8_t  vmaj, vmin;
    uint8_t  oss, iss, bss;            /* число выходных / входных / двунаправленных потоков */
    uint8_t  ok64;
    uint8_t  ncodecs;
    struct audio_codec codec[AUDIO_CODECS];
};

unsigned audio_count(void);
int      audio_get(unsigned idx, struct audio_info *out);
void     audio_print_all(void);

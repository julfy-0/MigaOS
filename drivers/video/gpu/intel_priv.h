/* Внутреннее состояние драйвера Intel. */
#pragma once
#include <miga/igfx.h>
#include <miga/gpu.h>

struct igfx_dev {
    volatile uint8_t *mmio;
    uint64_t mmio_size;
    uint8_t  bus, dev, fn;
    uint16_t did;
    uint8_t  ver;                  /* версия display engine: 6..14, 20, 30 */
    const char *name;
    volatile uint8_t *gtt;         /* GGTT внутри BAR0, NULL = недоступна */
    uint64_t gtt_bytes;
    struct igfx_head head[IGFX_MAX_HEADS];
    unsigned nheads;
    uint8_t  edid[256];
    unsigned edid_len;
    int ok;
};
extern struct igfx_dev IG;

static inline uint32_t igr(uint32_t off)
{
    if (!IG.mmio || off + 4 > IG.mmio_size) return 0xFFFFFFFFu;
    return *(volatile uint32_t *)(IG.mmio + off);
}
static inline void igw(uint32_t off, uint32_t v)
{
    if (IG.mmio && off + 4 <= IG.mmio_size) *(volatile uint32_t *)(IG.mmio + off) = v;
}

int  igfx_ddc_read_edid(void);     /* intel_ddc.c: заполняет IG.edid/edid_len */
void igfx_disp_init(void);         /* intel_disp.c: IG.head[], IG.nheads */

#include <kernel/initrd.h>

static const uint8_t *g_base;
static uint64_t g_size;
static uint32_t g_count;

int initrd_init(const void *base, uint64_t size)
{
    const struct initrd_hdr *h = base;
    const struct initrd_ent *e;
    uint64_t tbl_end;

    g_base = 0; g_count = 0; g_size = 0;
    if (!base || size < sizeof *h) return -1;
    for (int i = 0; i < 8; i++)
        if (h->magic[i] != INITRD_MAGIC[i]) return -2;
    if (h->count == 0 || h->count > INITRD_MAX_ENT) return -3;
    if (h->total > size) return -4;
    tbl_end = sizeof *h + (uint64_t)h->count * sizeof *e;
    if (tbl_end > size) return -5;

    e = (const struct initrd_ent *)((const uint8_t *)base + sizeof *h);
    for (uint32_t i = 0; i < h->count; i++) {
        int term = 0;
        for (int k = 0; k < INITRD_NAME_MAX; k++) if (!e[i].name[k]) { term = k > 0; break; }
        if (!term) return -6;                          /* пустое имя или нет NUL */
        if (e[i].off < tbl_end || e[i].off > size) return -7;
        if (e[i].size > size - e[i].off) return -8;
    }
    g_base = base; g_size = size; g_count = h->count;
    return 0;
}

int initrd_find(const char *name, const void **data, uint64_t *size)
{
    const struct initrd_ent *e;
    if (!g_base || !name) return -1;
    e = (const struct initrd_ent *)(g_base + sizeof(struct initrd_hdr));
    for (uint32_t i = 0; i < g_count; i++) {
        const char *a = e[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) {
            if (data) *data = g_base + e[i].off;
            if (size) *size = e[i].size;
            return 0;
        }
    }
    return -1;
}

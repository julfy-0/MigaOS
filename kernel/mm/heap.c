/* Куча ядра поверх PMM. Цель этой версии: простота и надёжность, а не скорость.
 *
 * Устройство:
 *  - Малые блоки (до 1024 байт) выдаются из «слэбов»: одна страница PMM = один слэб одного размерного класса
 *    (16, 32, 64, 128, 256, 512, 1024). В начале страницы заголовок (64 байта), дальше равные объекты.
 *    Свободные объекты связаны в список прямо внутри себя.
 *  - Крупные блоки (больше 1024 байт) = непрерывный диапазон страниц PMM с тем же заголовком в первой странице.
 *  - Любой указатель kmalloc лежит в первой странице своего блока, поэтому kfree находит заголовок как (p & ~4095).
 *  - Один процессор: защита от прерываний через cpu_irq_save/cpu_irq_restore.
 *
 * Отладочные проверки (всегда включены, это дёшево): магия заголовка, выравнивание указателя внутри слэба,
 * двойное освобождение, запись в освобождённый объект (отравленные байты 0xDD проверяются при выдаче).
 */
#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <kernel/mm.h>
#include <miga/string.h>

#define HDR_MAGIC    0x4B48454Fu
#define HDR_SIZE     64u
#define SMALL_MAX    1024u
#define LARGE_MAX    (1ULL << 32)                 /* больше 4 ГиБ не даём: заведомо ошибка вызывающего */
#define POISON_BYTE  0xDD
#define POISON_WORD  0xDDDDDDDDDDDDDDDDULL
#define FREE_MAGIC   0xF4EEF4EEF4EEF4EEULL

#define NCLASS 7
static const uint16_t class_size[NCLASS] = {16, 32, 64, 128, 256, 512, 1024};

enum { KIND_SLAB = 1, KIND_LARGE = 2 };

struct hdr {
    uint32_t magic;
    uint16_t kind;
    uint16_t cls;           /* слэб: номер класса */
    uint32_t used;          /* слэб: живых объектов */
    uint32_t cap;           /* слэб: вместимость; крупный блок: число страниц */
    uint64_t req;           /* крупный блок: запрошенный размер */
    void *free;             /* слэб: список свободных объектов */
    struct hdr *next, *prev;/* список слэбов класса, где есть свободные объекты */
};
_Static_assert(sizeof(struct hdr) <= HDR_SIZE, "slab header too large");

/* Свободный объект. Все классы >= 16 байт, поэтому оба поля помещаются. */
struct fobj {
    struct fobj *next;
    uint64_t magic;         /* FREE_MAGIC, пока объект свободен; при выдаче обнуляется */
};

static struct hdr *partial[NCLASS];
static struct kheap_stats st;

static void list_push(struct hdr *h)
{
    h->prev = 0;
    h->next = partial[h->cls];
    if (h->next) h->next->prev = h;
    partial[h->cls] = h;
}

static void list_remove(struct hdr *h)
{
    if (h->prev) h->prev->next = h->next; else partial[h->cls] = h->next;
    if (h->next) h->next->prev = h->prev;
    h->next = h->prev = 0;
}

static unsigned class_for(size_t n)
{
    unsigned c = 0;
    while (class_size[c] < n) c++;
    return c;
}

static struct hdr *slab_new(unsigned cls)
{
    uint64_t pa = pmm_alloc();
    if (!pa) return 0;
    struct hdr *h = P2V(pa);
    uint32_t size = class_size[cls];
    h->magic = HDR_MAGIC;
    h->kind = KIND_SLAB;
    h->cls = (uint16_t)cls;
    h->used = 0;
    h->cap = (uint32_t)((PAGE_SIZE - HDR_SIZE) / size);
    h->req = 0;
    h->free = 0;
    h->next = h->prev = 0;
    char *base = (char *)h + HDR_SIZE;
    memset(base, POISON_BYTE, h->cap * size);
    for (uint32_t i = h->cap; i-- > 0;) {          /* по убыванию: голова списка = первый объект */
        struct fobj *o = (struct fobj *)(base + (size_t)i * size);
        o->next = h->free;
        o->magic = FREE_MAGIC;
        h->free = o;
    }
    st.slab_pages++;
    return h;
}

static void *small_alloc(unsigned cls)
{
    struct hdr *h = partial[cls];
    if (!h) {
        h = slab_new(cls);
        if (!h) return 0;
        list_push(h);
    }
    struct fobj *o = h->free;
    uint32_t size = class_size[cls];
    /* Объект должен быть ровно таким, каким мы его отравили при освобождении. */
    for (uint32_t i = 16; i < size; i += 8)
        if (*(uint64_t *)((char *)o + i) != POISON_WORD)
            panic("kmalloc: write to freed object %p (class %u)", (void *)o, (unsigned)size);
    h->free = o->next;
    h->used++;
    if (!h->free) list_remove(h);                  /* слэб заполнен: из списка свободных выходит */
    o->magic = 0;
    st.live_allocs++;
    st.live_bytes += size;
    st.total_allocs++;
    return o;
}

static void *large_alloc(size_t n)
{
    uint64_t pages = ((uint64_t)n + HDR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pa = pages == 1 ? pmm_alloc() : pmm_alloc_contig(pages);
    if (!pa) return 0;
    struct hdr *h = P2V(pa);
    h->magic = HDR_MAGIC;
    h->kind = KIND_LARGE;
    h->cls = 0;
    h->used = 1;
    h->cap = (uint32_t)pages;
    h->req = n;
    h->free = 0;
    h->next = h->prev = 0;
    st.live_allocs++;
    st.live_bytes += pages * PAGE_SIZE - HDR_SIZE;
    st.total_allocs++;
    st.large_pages += pages;
    return (char *)h + HDR_SIZE;
}

void *kmalloc(size_t n)
{
    if (n > LARGE_MAX) return 0;
    if (n == 0) n = 1;
    uint64_t fl = cpu_irq_save();
    void *r = n <= SMALL_MAX ? small_alloc(class_for(n)) : large_alloc(n);
    cpu_irq_restore(fl);
    return r;
}

void *kzalloc(size_t n)
{
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

static void small_free(struct hdr *h, void *p)
{
    uint32_t size = class_size[h->cls];
    uint64_t off = (uint64_t)((char *)p - (char *)h);
    if (off < HDR_SIZE || (off - HDR_SIZE) % size || (off - HDR_SIZE) / size >= h->cap)
        panic("kfree: %p is not an object boundary (class %u)", p, (unsigned)size);
    struct fobj *o = p;
    if (o->magic == FREE_MAGIC)                    /* возможно двойное освобождение; данные пользователя могли совпасть, проверяем список */
        for (struct fobj *f = h->free; f; f = f->next)
            if (f == o) panic("kfree: double free of %p (class %u)", p, (unsigned)size);

    int was_full = (h->free == 0);
    memset(p, POISON_BYTE, size);
    o->next = h->free;
    o->magic = FREE_MAGIC;
    h->free = o;
    h->used--;
    if (was_full) list_push(h);

    st.live_allocs--;
    st.live_bytes -= size;
    st.total_frees++;

    /* Пустой слэб отдаём PMM, если у класса есть другой слэб; один пустой остаётся в запасе (чтобы не дёргать PMM на каждый free/malloc). */
    if (h->used == 0 && (h->prev || h->next)) {
        list_remove(h);
        h->magic = 0;
        pmm_free(V2P(h));
        st.slab_pages--;
    }
}

static void large_free(struct hdr *h, void *p)
{
    if ((char *)p != (char *)h + HDR_SIZE) panic("kfree: %p is not the start of a large block", p);
    uint64_t pages = h->cap;
    st.live_allocs--;
    st.live_bytes -= pages * PAGE_SIZE - HDR_SIZE;
    st.total_frees++;
    st.large_pages -= pages;
    h->magic = 0;                                  /* повторный kfree увидит чужую страницу и сработает panic */
    for (uint64_t i = 0; i < pages; i++) pmm_free(V2P(h) + i * PAGE_SIZE);
}

void kfree(void *p)
{
    if (!p) return;
    uint64_t fl = cpu_irq_save();
    struct hdr *h = (struct hdr *)((uintptr_t)p & ~(uintptr_t)(PAGE_SIZE - 1));
    if (h->magic != HDR_MAGIC) panic("kfree: %p was not allocated by kmalloc (or is a double free)", p);
    if (h->kind == KIND_SLAB) small_free(h, p);
    else if (h->kind == KIND_LARGE) large_free(h, p);
    else panic("kfree: corrupted header for %p", p);
    cpu_irq_restore(fl);
}

void kheap_stats_get(struct kheap_stats *s)
{
    uint64_t fl = cpu_irq_save();
    *s = st;
    cpu_irq_restore(fl);
}

void kheap_trim(void)
{
    uint64_t fl = cpu_irq_save();
    for (unsigned c = 0; c < NCLASS; c++) {
        struct hdr *h = partial[c];
        while (h) {
            struct hdr *next = h->next;
            if (h->used == 0) {
                list_remove(h);
                h->magic = 0;
                pmm_free(V2P(h));
                st.slab_pages--;
            }
            h = next;
        }
    }
    cpu_irq_restore(fl);
}

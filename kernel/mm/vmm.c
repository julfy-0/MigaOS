/* Адресные пространства: создание/уничтожение, отображение и снятие отображения страниц 4 КиБ.
 * Раскладка и контракт описаны в kernel/include/kernel/mm.h.
 *
 * Общая ядерная часть. PML4 каждого пространства при создании получает копии ядерных записей главной PML4
 * (paging.c). Нижние уровни (PDPT/PD/PT) общие по указателю, поэтому правки внутри них видны везде сразу.
 * Единственное, что надо синхронизировать, это появление НОВОЙ записи PML4 (mmio_map для BAR выше 512 ГиБ,
 * vm_map по адресу в верхней половине): для этого все пространства, созданные vm_space_create, лежат в списке,
 * и vm_kernel_pml4e_set вписывает запись в каждое.
 */
#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <kernel/mm.h>
#include <miga/string.h>

#define P_PRESENT 1ULL
#define P_RW      2ULL
#define P_US      4ULL
#define P_HUGE    0x80ULL
#define P_NX      (1ULL << 63)
#define ADDR_MASK 0x000FFFFFFFFFF000ULL
#define ADDR_2M   0x000FFFFFFFE00000ULL
#define ADDR_1G   0x000FFFFFC0000000ULL

#define PML4_USER_FIRST 128
#define PML4_USER_LAST  255

#ifdef MM_HOSTTEST
#define invlpg(va) ((void)(va))
#else
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }
#endif

struct space_node {
    struct space_node *next;
    uint64_t cr3;
};
static struct space_node *spaces;

static inline int is_user_idx(unsigned i) { return i >= PML4_USER_FIRST && i <= PML4_USER_LAST; }
static inline int canonical(uint64_t va)
{
    uint64_t top = va >> 47;                       /* биты 63..47 обязаны совпадать */
    return top == 0 || top == 0x1FFFF;
}

static uint64_t *new_table(void)
{
    uint64_t pa = pmm_alloc();
    if (!pa) return 0;
    memset(P2V(pa), 0, PAGE_SIZE);
    return P2V(pa);
}

/* ------------------------------------------------------------------ синхронизация ядерных записей PML4 */
static void pml4e_set_locked(unsigned idx, uint64_t val)
{
    if (idx >= 512 || is_user_idx(idx)) panic("vm: PML4[%u] is not a kernel entry", idx);
    ((uint64_t *)P2V(vm_kernel_cr3()))[idx] = val;
    for (struct space_node *n = spaces; n; n = n->next) ((uint64_t *)P2V(n->cr3))[idx] = val;
}

void vm_kernel_pml4e_set(unsigned idx, uint64_t val)
{
    uint64_t fl = cpu_irq_save();
    pml4e_set_locked(idx, val);
    cpu_irq_restore(fl);
}

/* ------------------------------------------------------------------ создание / уничтожение */
uint64_t vm_space_create(void)
{
    struct space_node *n = kmalloc(sizeof *n);
    if (!n) return 0;
    uint64_t pa = pmm_alloc();
    if (!pa) { kfree(n); return 0; }
    uint64_t *t = P2V(pa);
    uint64_t fl = cpu_irq_save();
    const uint64_t *m = P2V(vm_kernel_cr3());
    for (unsigned i = 0; i < 512; i++) t[i] = is_user_idx(i) ? 0 : m[i];
    n->cr3 = pa;
    n->next = spaces;
    spaces = n;
    cpu_irq_restore(fl);
    return pa;
}

static void free_tree(uint64_t table, int level, unsigned flags)
{
    uint64_t *t = P2V(table);
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & P_PRESENT)) continue;
        uint64_t c = e & ADDR_MASK;
        if (level > 1) free_tree(c, level - 1, flags);
        else if (flags & VM_FREE_FRAMES) pmm_free(c);
    }
    pmm_free(table);
}

void vm_space_destroy(uint64_t cr3, unsigned flags)
{
    cr3 &= ADDR_MASK;
    if (!cr3) return;
    if (cr3 == vm_kernel_cr3()) panic("vm: attempt to destroy the kernel address space");
    if ((cpu_read_cr3() & ADDR_MASK) == cr3) panic("vm: attempt to destroy the active address space");

    uint64_t fl = cpu_irq_save();
    struct space_node **pp = &spaces;
    while (*pp && (*pp)->cr3 != cr3) pp = &(*pp)->next;
    struct space_node *n = *pp;
    if (!n) { cpu_irq_restore(fl); panic("vm: %#lx was not created by vm_space_create", cr3); }
    *pp = n->next;
    cpu_irq_restore(fl);

    uint64_t *pml4 = P2V(cr3);
    for (unsigned i = PML4_USER_FIRST; i <= PML4_USER_LAST; i++)
        if (pml4[i] & P_PRESENT) free_tree(pml4[i] & ADDR_MASK, 3, flags);
    pmm_free(cr3);                                 /* ядерные записи общие, их таблицы не трогаем */
    kfree(n);
}

/* ------------------------------------------------------------------ обход таблиц */
static void split(uint64_t va, unsigned idx[4])
{
    idx[0] = (va >> 39) & 511; idx[1] = (va >> 30) & 511; idx[2] = (va >> 21) & 511; idx[3] = (va >> 12) & 511;
}

/* Находит PTE страницы va. create=1 создаёт недостающие таблицы.
 * Возврат: указатель на PTE или NULL; *err = VM_E_NOENT / VM_E_NOMEM / VM_E_INVAL (на пути huge-страница). */
static uint64_t *walk(uint64_t cr3, uint64_t va, int create, int user, int *err)
{
    unsigned idx[4];
    split(va, idx);
    uint64_t *t = P2V(cr3 & ADDR_MASK);
    for (int l = 0; l < 3; l++) {
        uint64_t *e = &t[idx[l]];
        if (!(*e & P_PRESENT)) {
            if (!create) { *err = VM_E_NOENT; return 0; }
            uint64_t *n = new_table();
            if (!n) { *err = VM_E_NOMEM; return 0; }
            if (l == 0 && !is_user_idx(idx[0])) {
                uint64_t v = V2P(n) | P_PRESENT | P_RW;
                pml4e_set_locked(idx[0], v);                             /* ядерная запись: сразу во все пространства */
                *e = v;                                                  /* и в это, даже если оно создано не через vm_space_create */
            } else {
                *e = V2P(n) | P_PRESENT | P_RW | (user ? P_US : 0);      /* U/S должен стоять на КАЖДОМ уровне */
            }
        } else if (l > 0 && (*e & P_HUGE)) {
            *err = VM_E_INVAL;
            return 0;
        }
        t = P2V(*e & ADDR_MASK);
    }
    return &t[idx[3]];
}

static uint64_t pte_from_flags(uint64_t pa, unsigned flags)
{
    uint64_t e = pa | P_PRESENT;
    if (flags & VM_WRITE) e |= P_RW;
    if (flags & VM_USER) e |= P_US;
    if ((flags & VM_NOEXEC) && vm_nx_enabled()) e |= P_NX;
    return e;
}

static unsigned flags_from_pte(uint64_t e)
{
    unsigned f = 0;
    if (e & P_RW) f |= VM_WRITE;
    if (e & P_US) f |= VM_USER;
    if (e & P_NX) f |= VM_NOEXEC;
    return f;
}

/* ------------------------------------------------------------------ map / unmap / query */
int vm_map(uint64_t cr3, uint64_t va, uint64_t pa, unsigned flags)
{
    if ((va | pa) & (PAGE_SIZE - 1) || pa >> 52) return VM_E_INVAL;
    if (flags & ~(VM_WRITE | VM_USER | VM_NOEXEC)) return VM_E_INVAL;
    if (flags & VM_USER) { if (va < VM_USER_BASE || va >= VM_USER_END) return VM_E_INVAL; }
    else if (va < VM_KERNEL_BASE) return VM_E_INVAL;     /* ядерные страницы живут только в верхней половине */

    uint64_t fl = cpu_irq_save();
    int err = 0, rc;
    uint64_t *pte = walk(cr3, va, 1, !!(flags & VM_USER), &err);
    if (!pte) rc = err;
    else if (*pte & P_PRESENT) rc = VM_E_EXIST;
    else { *pte = pte_from_flags(pa, flags); rc = VM_OK; }
    cpu_irq_restore(fl);
    return rc;
}

int vm_unmap(uint64_t cr3, uint64_t va, uint64_t *pa_out)
{
    if ((va & (PAGE_SIZE - 1)) || !canonical(va)) return VM_E_INVAL;
    uint64_t fl = cpu_irq_save();
    int err = 0, rc;
    uint64_t *pte = walk(cr3, va, 0, 0, &err);
    if (!pte) rc = err;
    else if (!(*pte & P_PRESENT)) rc = VM_E_NOENT;
    else {
        if (pa_out) *pa_out = *pte & ADDR_MASK;
        *pte = 0;
        invlpg(va);
        rc = VM_OK;
    }
    cpu_irq_restore(fl);
    return rc;
}

int vm_query(uint64_t cr3, uint64_t va, uint64_t *pa_out, unsigned *flags_out)
{
    if (!canonical(va)) return VM_E_INVAL;
    unsigned idx[4];
    split(va, idx);
    uint64_t fl = cpu_irq_save();
    uint64_t e = 0, pa = 0;
    int rc = VM_E_NOENT;
    uint64_t *t = P2V(cr3 & ADDR_MASK);
    for (int l = 0; l < 4; l++) {
        e = t[idx[l]];
        if (!(e & P_PRESENT)) break;
        if (l == 1 && (e & P_HUGE)) { pa = (e & ADDR_1G) | (va & 0x3FFFF000ULL); rc = VM_OK; break; }
        if (l == 2 && (e & P_HUGE)) { pa = (e & ADDR_2M) | (va & 0x1FF000ULL); rc = VM_OK; break; }
        if (l == 3) { pa = e & ADDR_MASK; rc = VM_OK; break; }
        t = P2V(e & ADDR_MASK);
    }
    cpu_irq_restore(fl);
    if (rc != VM_OK) return rc;
    if (pa_out) *pa_out = pa;
    if (flags_out) *flags_out = flags_from_pte(e);
    return VM_OK;
}

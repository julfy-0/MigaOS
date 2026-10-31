/* MigaBoot: UEFI-загрузчик MigaOS.
 *  1. настраивает графику (GOP)
 *  2. читает \EFI\MIGA\kernel.elf с тома, с которого загрузился сам
 *  3. раскладывает PT_LOAD сегменты по физическим адресам
 *  4. собирает BootInfo (framebuffer, карта памяти, RSDP)
 *  5. ExitBootServices и прыжок в ядро (SysV ABI, RDI = BootInfo*) */
#include "uefi.h"
#include "elf.h"
#include <miga/boot_info.h>

#define PAGE 4096ULL

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

/* ---------- вывод ---------- */
static void puts_(const char *s)
{
    CHAR16 b[3];
    for (; *s; s++) {
        if (*s == '\n') { b[0] = '\r'; b[1] = 0; ST->ConOut->OutputString(ST->ConOut, b); }
        b[0] = (unsigned char)*s; b[1] = 0;
        ST->ConOut->OutputString(ST->ConOut, b);
    }
}

static void puthex(uint64_t v)
{
    char b[19] = "0x";
    for (int i = 0; i < 16; i++) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xF];
    b[18] = 0;
    puts_(b);
}

static EFI_STATUS die(EFI_STATUS st, const char *msg)
{
    puts_("\nMigaBoot fatal: ");
    puts_(msg);
    puts_(" status=");
    puthex(st);
    puts_("\n");
    BS->Stall(8000000);
    return st;
}

/* ---------- мини-libc ---------- */
void *memcpy(void *d, const void *s, size_t n) { void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n)::"memory"); return r; }
void *memset(void *d, int c, size_t n) { void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }
static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

/* ---------- EDID монитора: копия в MEM_LOADER, оригинал после ExitBootServices пропадёт ---------- */
static void get_edid(struct boot_info *bi)
{
    EFI_GUID ga = EFI_EDID_ACTIVE_GUID, gd = EFI_EDID_DISCOVERED_GUID;
    EFI_EDID_PROTOCOL *e = 0;
    if (EFI_ERROR(BS->LocateProtocol(&ga, 0, (void **)&e)) || !e || !e->Edid || e->SizeOfEdid < 128) {
        e = 0;
        if (EFI_ERROR(BS->LocateProtocol(&gd, 0, (void **)&e)) || !e || !e->Edid || e->SizeOfEdid < 128) {
            puts_("EDID: not provided\n");
            return;
        }
    }
    uint32_t n = e->SizeOfEdid > 512 ? 512 : e->SizeOfEdid;
    uint8_t *copy;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, n, (void **)&copy))) return;
    for (uint32_t i = 0; i < n; i++) copy[i] = e->Edid[i];
    bi->edid = (uint64_t)copy; bi->edid_size = n;
}

/* ---------- графика ---------- */
static void setup_gop(struct boot_fb *fb, const struct boot_info *bi)
{
    /* родное разрешение монитора из первого DTD EDID (0 = неизвестно) */
    uint32_t nw = 0, nh = 0;
    if (bi->edid && bi->edid_size >= 128) {
        const uint8_t *e = (const uint8_t *)bi->edid, *p = e + 54;
        if (p[0] || p[1]) { nw = p[2] | (p[4] >> 4) << 8; nh = p[5] | (p[7] >> 4) << 8; }
    }
    EFI_GUID g = EFI_GOP_GUID;
    EFI_GOP *gop = 0;
    memset(fb, 0, sizeof *fb);
    if (EFI_ERROR(BS->LocateProtocol(&g, 0, (void **)&gop)) || !gop) { puts_("GOP: not found\n"); return; }

    /* Режим: родное разрешение монитора (EDID); иначе самый большой линейный режим до 4096x2304 (не меньше 1024x768).
     * Раньше было жёстко 1024x768: на современном мониторе это мыло и не 240 Гц-тайминг. Частоту обновления GOP не
     * показывает: её выбирает прошивка для выбранного режима (обычно предпочтительный тайминг EDID). */
    uint32_t best = ~0u, bestpx = 0;
    for (uint32_t m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN sz; EFI_GOP_MODE_INFO *mi;
        if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &mi))) continue;
        if (mi->PixelFormat == PixelBltOnly || mi->PixelFormat == PixelBitMask) continue;
        uint32_t w = mi->HorizontalResolution, h = mi->VerticalResolution;
        if (nw && w == nw && h == nh) { best = m; break; }
        if (w > (nw ? 4096u : 1920u) || h > (nw ? 2304u : 1080u) || w < 1024 || h < 768) continue;   /* без EDID: до 1080p */
        if ((uint64_t)w * h > bestpx) { bestpx = (uint64_t)w * h; best = m; }
    }
    if (best != ~0u && best != gop->Mode->Mode) gop->SetMode(gop, best);

    EFI_GOP_MODE *md = gop->Mode;
    if (md->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) fb->format = FB_RGBX;
    else if (md->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) fb->format = FB_BGRX;
    else if (md->Info->PixelFormat == PixelBitMask) fb->format = (md->Info->PixelInformation.RedMask == 0xFF) ? FB_RGBX : FB_BGRX;
    else { puts_("GOP: BltOnly, no linear framebuffer\n"); return; }

    fb->base = md->FrameBufferBase;
    fb->size = md->FrameBufferSize;
    fb->width = md->Info->HorizontalResolution;
    fb->height = md->Info->VerticalResolution;
    fb->pitch = md->Info->PixelsPerScanLine * 4;
}

/* ---------- чтение файла ---------- */
static EFI_STATUS read_file(EFI_HANDLE image, CHAR16 *path, void **out, UINTN *out_size)
{
    EFI_GUID lg = EFI_LOADED_IMAGE_GUID, sg = EFI_SFS_GUID;
    EFI_LOADED_IMAGE *li; EFI_SIMPLE_FILE_SYSTEM *fs; EFI_FILE *root, *f;
    EFI_STATUS st;

    if (EFI_ERROR(st = BS->HandleProtocol(image, &lg, (void **)&li))) return st;
    if (EFI_ERROR(st = BS->HandleProtocol(li->DeviceHandle, &sg, (void **)&fs))) return st;
    if (EFI_ERROR(st = fs->OpenVolume(fs, &root))) return st;
    if (EFI_ERROR(st = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0))) return st;

    uint64_t size = 0;
    f->SetPosition(f, ~0ULL);
    f->GetPosition(f, &size);
    f->SetPosition(f, 0);

    void *buf;
    if (EFI_ERROR(st = BS->AllocatePool(EfiLoaderData, size, &buf))) return st;
    UINTN n = size;
    if (EFI_ERROR(st = f->Read(f, &n, buf))) return st;
    f->Close(f); root->Close(root);
    *out = buf; *out_size = n;
    return EFI_SUCCESS;
}

/* ---------- ELF ---------- */
static EFI_STATUS load_elf(void *img, UINTN size, uint64_t *entry, uint64_t *base, uint64_t *len)
{
    Elf64_Ehdr *eh = img;
    if (size < sizeof *eh || eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' || eh->e_ident[4] != 2 ||
        eh->e_machine != EM_X86_64 || eh->e_type != ET_EXEC)
        return EFI_UNSUPPORTED;

    Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)img + eh->e_phoff);
    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > size) return EFI_LOAD_ERROR;
        if (ph[i].p_paddr < lo) lo = ph[i].p_paddr;
        if (ph[i].p_paddr + ph[i].p_memsz > hi) hi = ph[i].p_paddr + ph[i].p_memsz;
    }
    if (lo >= hi) return EFI_LOAD_ERROR;
    lo &= ~(PAGE - 1);
    hi = (hi + PAGE - 1) & ~(PAGE - 1);

    EFI_PHYSICAL_ADDRESS addr = lo;
    EFI_STATUS st = BS->AllocatePages(AllocateAddress, EfiLoaderData, (hi - lo) / PAGE, &addr);
    if (EFI_ERROR(st)) return st;
    memset((void *)lo, 0, hi - lo);

    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD)
            memcpy((void *)ph[i].p_paddr, (uint8_t *)img + ph[i].p_offset, ph[i].p_filesz);

    *entry = eh->e_entry; *base = lo; *len = hi - lo;
    return EFI_SUCCESS;
}

/* ---------- карта памяти ---------- */
static uint32_t convert_type(uint32_t t)
{
    switch (t) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:      return MEM_USABLE;     /* после ExitBootServices свободны */
    case EfiLoaderCode:
    case EfiLoaderData:            return MEM_LOADER;
    case EfiACPIReclaimMemory:     return MEM_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:         return MEM_ACPI_NVS;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace: return MEM_MMIO;
    case EfiUnusableMemory:        return MEM_BAD;
    default:                       return MEM_RESERVED;   /* runtime services, PAL, reserved */
    }
}

typedef void (__attribute__((sysv_abi)) *kernel_entry_t)(struct boot_info *);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st; BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, 0);
    ST->ConOut->ClearScreen(ST->ConOut);
    puts_("MigaBoot 0.1 - UEFI loader for MigaOS\n");

    struct boot_info *bi;
    EFI_STATUS s;
    if (EFI_ERROR(s = BS->AllocatePool(EfiLoaderData, sizeof *bi, (void **)&bi))) return die(s, "alloc bootinfo");
    memset(bi, 0, sizeof *bi);
    bi->magic = MIGA_BOOT_MAGIC;
    bi->version = 1;

    get_edid(bi);
    setup_gop(&bi->fb, bi);

    void *file; UINTN fsize;
    puts_("Loading \\EFI\\MIGA\\kernel.elf ...\n");
    if (EFI_ERROR(s = read_file(image, (CHAR16 *)u"\\EFI\\MIGA\\kernel.elf", &file, &fsize)))
        return die(s, "cannot read kernel.elf");

    uint64_t entry, kbase, klen;
    if (EFI_ERROR(s = load_elf(file, fsize, &entry, &kbase, &klen))) return die(s, "bad kernel ELF");
    bi->kernel_base = kbase; bi->kernel_size = klen;
    puts_("Kernel at "); puthex(kbase); puts_(", entry "); puthex(entry); puts_("\n");

    /* RSDP из таблицы конфигурации (предпочитаем ACPI 2.0) */
    EFI_GUID a2 = EFI_ACPI20_GUID, a1 = EFI_ACPI10_GUID;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_eq(&t->VendorGuid, &a2)) { bi->rsdp = (uint64_t)t->VendorTable; break; }
        if (guid_eq(&t->VendorGuid, &a1) && !bi->rsdp) bi->rsdp = (uint64_t)t->VendorTable;
    }

    /* SMBIOS: 3.x (64-битный) предпочтительнее 2.x */
    EFI_GUID sm3 = EFI_SMBIOS3_GUID, sm2 = EFI_SMBIOS_GUID;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_eq(&t->VendorGuid, &sm3)) { bi->smbios = (uint64_t)t->VendorTable; break; }
        if (guid_eq(&t->VendorGuid, &sm2) && !bi->smbios) bi->smbios = (uint64_t)t->VendorTable;
    }

    /* Буферы под карту памяти выделяем ДО последнего GetMemoryMap */
    UINTN map_size = 0, map_key, desc_size; uint32_t desc_ver;
    BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    UINTN cap = map_size + 16 * desc_size;
    EFI_MEMORY_DESCRIPTOR *raw;
    struct mem_region *regs;
    if (EFI_ERROR(s = BS->AllocatePool(EfiLoaderData, cap, (void **)&raw))) return die(s, "alloc map");
    if (EFI_ERROR(s = BS->AllocatePool(EfiLoaderData, (cap / desc_size + 1) * sizeof *regs, (void **)&regs)))
        return die(s, "alloc regions");

    puts_("Exiting boot services...\n");
    for (int tries = 0;; tries++) {
        map_size = cap;
        if (EFI_ERROR(s = BS->GetMemoryMap(&map_size, raw, &map_key, &desc_size, &desc_ver))) return die(s, "GetMemoryMap");
        s = BS->ExitBootServices(image, map_key);
        if (!EFI_ERROR(s)) break;
        if (tries >= 3) return die(s, "ExitBootServices");
    }
    __asm__ volatile("cli");
    /* С этого момента UEFI Boot Services недоступны: ни печати, ни аллокаций. */

    uint64_t n = 0;
    for (UINTN off = 0; off + desc_size <= map_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)raw + off);
        uint32_t t = convert_type(d->Type);
        uint64_t len = d->NumberOfPages * PAGE;
        if (n && regs[n - 1].type == t && regs[n - 1].base + regs[n - 1].length == d->PhysicalStart) {
            regs[n - 1].length += len; /* склеиваем соседей одного типа */
        } else {
            regs[n].base = d->PhysicalStart; regs[n].length = len; regs[n].type = t; regs[n]._pad = 0;
            n++;
        }
    }
    bi->mem = regs; bi->mem_count = n;

    ((kernel_entry_t)entry)(bi);
    for (;;) __asm__ volatile("hlt");
}

# C5: что здесь и куда это встраивать

Файлы лежат по путям репозитория. Проверено на хосте (ASan+UBSan, фаззинг 200k портчей ELF и 100k портчей initrd):
elf_load на реальном hello.elf даёт 2 страницы: 0x400000 R+X, 0x401000 R; entry=0x400000.

## Нужно сделать руками (я не видел эти исходники)
1. boot_info.h (через C1): добавить `uint64_t initrd_base, initrd_size;` (0 = initrd нет).
2. boot/uefi/main.c: рядом с чтением kernel.elf прочитать \EFI\MIGA\initrd.img тем же кодом
   (вынести `load_file(path, &buf, &size)`), память взять AllocatePages(AllocateAnyPages, <тип как у kernel.elf>),
   записать в boot_info до ExitBootServices. Страницы должны попасть в карте памяти ядра в тип, который PMM
   НЕ отдаёт аллокатору (по README освобождается BootServicesData; MEM_LOADER пока не освобождается;
   лучше завести MEM_INITRD).
3. kernel/core/elf.c, верх файла: три extern-а (pmm_alloc_page, phys_to_virt, vmm_map_user) заменить реальными
   функциями C2/C3. Если в таблицах страниц не включён EFER.NXE, бит NX молча игнорируется и R/RW/RX
   не будут различаться: проверь paging.c.
4. system/init/syscall.h: номера, инструкция (syscall) и регистры подогнать под C4.
5. Ядро запускает init (в main.c после initrd_init), порядок:
   initrd_init(bi->initrd_base, bi->initrd_size); initrd_find("init") -> новый процесс с as ->
   elf_load -> стек пользователя (например 4 страницы под 0x7ffffffff000) -> caps = CAP_ALL -> в планировщик.
6. Ядро-сторона syscalls: SYS_WRITE(fd,buf,n) требует CAP_CONSOLE и пишет в serial (копировать из user-памяти
   с проверкой диапазона < 0x800000000000); SYS_SPAWN(name,caps) требует CAP_SPAWN и
   `(caps & ~current->caps) == 0`, иначе -EPERM (потомок не может получить больше прав родителя);
   SYS_INITRD_READ требует CAP_INITRD; SYS_WAIT требует CAP_SPAWN. Выход init = panic/halt.

Makefile: см. Makefile.c5.patch.txt.

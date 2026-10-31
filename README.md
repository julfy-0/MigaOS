# MigaOS

Учебная ОС для x86_64 с собственным UEFI-загрузчиком **MigaBoot**. Без gnu-efi и без GRUB: только `gcc` + `binutils`.

## Структура

```
MigaOS/
├── Makefile                  сборка всего: make / make run / make check
├── include/miga/             ОБЩИЙ КОНТРАКТ (единственное, что видят драйверы)
│   ├── boot_info.h             загрузчик → ядро (framebuffer, карта памяти, RSDP)
│   ├── driver.h                модель драйвера, макрос MIGA_DRIVER()
│   ├── console.h irq.h time.h input.h log.h io.h string.h   API ядра для драйверов
├── boot/uefi/                MigaBoot, UEFI-приложение (PE32+)
│   ├── crt0.S  reloc.c  link.lds   вход PE, самоперемещение, раскладка образа
│   ├── uefi.h  elf.h               минимальные определения UEFI и ELF
│   └── main.c                      GOP → загрузка kernel.elf → карта памяти → ExitBootServices → прыжок
├── kernel/                   ядро (знает про железо только через arch/)
│   ├── kernel.ld                   адрес 0x200000, сегменты R+X / R / R+W
│   ├── arch/x86_64/                entry.S, gdt.c, idt.c, isr.S, pic.c
│   ├── core/                       main, panic, printk, irq, console, time, input, driver (реестр), kshell
│   ├── mm/                         paging.c (свои таблицы страниц), pmm.c (битовая карта)
│   ├── lib/string.c
│   └── include/kernel/             приватные заголовки ядра
├── drivers/                  драйверы: отдельная библиотека libdrivers.a
│   ├── serial/uart16550.c          COM1, раннее логирование          (DRV_EARLY)
│   ├── video/fbcon/                текстовая консоль на GOP-фреймбуфере (DRV_EARLY)
│   ├── input/ps2kbd.c              PS/2-клавиатура, IRQ1              (DRV_NORMAL)
│   └── timer/pit.c                 PIT 100 Гц, системные тики         (DRV_NORMAL)
└── tools/                    genfont.py (шрифт), check_pe.py, mkimage.sh
```

## Граница ядро ↔ драйверы

- Драйверы компилируются с `-I include` и **не видят** `kernel/include`. Всё, что им нужно от ядра, объявлено в `include/miga/`.
- Драйвер не вызывается напрямую: он объявляет себя `MIGA_DRIVER(...)`, запись попадает в секцию `.miga_drivers`, ядро обходит её в `drivers_init()` по стадиям (`DRV_EARLY` → `DRV_NORMAL`).
- `make check` печатает неопределённые символы `libdrivers.a`: это и есть реальный API, которым драйверы пользуются (сейчас 6 функций).

Минимальный драйвер:

```c
#include <miga/driver.h>
static int hello_probe(const struct boot_info *bi) { /* init железа */ return 0; }
MIGA_DRIVER(hello, "hello", DRV_NORMAL, hello_probe);
```
Положите файл в `drivers/<класс>/`, больше ничего менять не надо.

## MigaGUI (графическая оболочка с GUI-сервером)

GUI стартует автоматически после загрузки и сразу открывает Terminal. Выход в kshell — `Ctrl+Q`, обратно — команда `gui`. Код целиком в `kernel/gui/` (Makefile подхватывает его сам).

```
kernel/include/kernel/gui.h   единственный заголовок, который видят приложения (API клиента, события, тема)
kernel/gui/server.c           GUI-СЕРВЕР: фреймбуфер, backbuffer, окна, z-порядок, фокус, рабочий стол, панель задач,
                              лаунчер, очереди запросов/событий, таймеры окон, композитор с dirty-rect
kernel/gui/client.c           клиентская библиотека: примитивы + запросы серверу (damage/title/tick/close)
kernel/gui/gfx.c, fmt.c       графика (fill/rect/line/text/blit) и мини-snprintf
kernel/gui/apps/              11 клиентов: terminal.c sysmon.c notepad.c calc.c devices.c snake.c paint.c mines.c dispcheck.c files.c settings.c
```
Клиент ↔ сервер: пиксели клиент рисует в общую поверхность окна; всё остальное — сообщения через очередь
запросов (`struct gui_req`) и очередь событий окна (`struct gui_event`). Когда появятся процессы, очереди станут IPC.

Клавиши сервера: `Esc` меню приложений, `1-9` запуск (10-е и 11-е приложение: меню, мышь или `run <имя>`), `Ctrl+N` следующее окно, `Ctrl+W` закрыть, `Ctrl+G` двигать окно
стрелками (Enter — готово), `Ctrl+Q` выйти в kshell. Мышь: PS/2 (`drivers/input/ps2mouse.c`, IRQ12, колесо) и USB HID boot-мышь (`usbhid`/`xhci`). Курсор — стрелка как в Windows,
рисуется прямо во фреймбуфер (кадр не пересобирается), ускорение как «повышенная точность указателя». Мышью: клик по окну — фокус,
перетаскивание за заголовок, красная кнопка закрытия, панель задач, меню MIGA, двойной клик по иконке, правая кнопка по столу — меню.
Приложения получают `GUI_EV_MOUSE`. Хабы USB не поддерживаются (мышь должна быть в порту контроллера напрямую).

**240 Гц:** загрузчик выбирает родное разрешение из EDID (без EDID — до 1080p), композитор перерисовывает только грязные
прямоугольники и держит темп кадров по TSC (период 4,17 мс). Частота берётся из EDID (все тайминги и Range Limits) или командой
`hz 240` в kshell; на панели задач видно «Hz / fps». Реальную частоту обновления монитора выставляет прошивка (GOP): драйверов модесета
GPU (AMD/Intel/NVIDIA) в проекте нет, они только определяют карту. Фреймбуфер отображается как write-combining (PAT).
Шрифт только ASCII (8x16), поэтому интерфейс на английском.

Чтобы добавить приложение: файл в `kernel/gui/apps/` с `const struct gui_app app_xxx`, строка в массиве `apps[]` в server.c.

## Как грузится

1. UEFI запускает `\EFI\BOOT\BOOTX64.EFI` (MigaBoot).
2. MigaBoot выбирает режим 1024x768 (GOP), читает `\EFI\MIGA\kernel.elf`, кладёт PT_LOAD-сегменты на их физические адреса.
3. Собирает `boot_info`, конвертирует карту памяти UEFI в свою, делает `ExitBootServices`.
4. Прыгает в `_start` ядра (SysV ABI, `RDI = boot_info*`).
5. Ядро: serial + fbcon → GDT → IDT → PIC → **свои таблицы страниц** → PMM → PIT + клавиатура → `sti` → kshell.

Свои таблицы страниц нужны до PMM: UEFI держит свои в памяти `BootServicesData`, а ядро отдаёт её аллокатору.

## Сборка и запуск

```sh
sudo apt install build-essential qemu-system-x86 ovmf mtools
make            # build/esp/ с BOOTX64.EFI и kernel.elf
make run        # QEMU + OVMF, логи ядра дублируются в терминал (-serial stdio)
make disk       # build/miga.img (FAT32, нужен mtools)
```
Если OVMF лежит в нестандартном месте: `make run OVMF=/path/to/OVMF.fd`.
Команды kshell: `help mem uptime clear reboot halt crash` (`crash` проверяет обработчик исключений).

## Статус и ограничения

- Всё собирается без предупреждений; PE-образ, таблица драйверов, заглушки прерываний и границы модулей проверены статически.
- **В QEMU эта версия ещё не запускалась** (в среде, где она писалась, не было QEMU/OVMF). Первый запуск покажет, что нужно подправить; если что-то не загрузится, пришлите вывод `-serial stdio`.
- Пока нет: ACPI/APIC (используется PIC), TSS/IST, USB (клавиатура только PS/2, на реальном железе без эмуляции не заработает), высокая половина адресного пространства, освобождение `MEM_LOADER`.

## Дорожная карта

1. APIC/IOAPIC + разбор ACPI (MADT), LAPIC-таймер вместо PIT
2. Куча ядра (`kmalloc`), VMM с отображением по запросу
3. Процессы, планировщик, TSS, user mode, syscall
4. Файловая система (initrd → FAT32), драйвер диска (AHCI/NVMe), USB HID
5. Shell и утилиты в user space


## Точки монтирования, Files и Settings

**VFS без файловой системы** (`kernel/core/vfs.c`, `kernel/include/kernel/vfs.h`): только таблица точек монтирования. Нет RAM-диска,
ramfs и initrd, нет драйверов ФС, файлы читать нельзя. Точка связывает путь с источником: `none`, `esp` или `diskN`.
`vfs_init()` (после драйверов) создаёт `/` (none, root), `/boot` (esp, ro, системная) и по `/mnt/diskN` на каждый найденный диск
(в состоянии idle: `mount /mnt/disk0` привязывает её к диску). Когда появится драйвер ФС, он подключится к `vfs_resolve()`:
тот возвращает самую глубокую смонтированную точку и остаток пути внутри неё.

**Настройки** (`kernel/core/settings.c`): фиксированная таблица в памяти ядра, значения живут до перезагрузки (писать на диск некуда).
Ключи: `sys.hostname`, `sys.user`, `term.blink`, `term.scrollstep`, `gui.hz` (то же, что команда `hz N`), `files.details`, `files.show_sys`.

**Приложения GUI:** `Files` (ходит по дереву точек, `M` монтирует, `U` отмонтирует, `I`/`H` переключают колонки и системные точки)
и `Settings` (bool: Enter/Left/Right, int: Left/Right и `+`/`-` по 10, текст: Enter, ввод, Enter, отмена Ctrl+C; `D` сброс одного, `R` всех).

**Новые команды** (`kernel/core/kcmds.c`, работают и в kshell, и в терминале GUI, попадают в `help` и Tab-дополнение):

```
pwd cd ls tree mount umount df stat lsblk      точки монтирования
settings set get reset                          настройки
hostname whoami uname ver free info             система
lsmod consoles lsusb lscpu lsmem echo expr cls
```
Примеры: `mount` · `mount /mnt/disk0` · `mount none /mnt/x -r -t test` · `umount /mnt/x` · `tree` · `set term.blink off` · `set gui.hz 144` · `expr 0x10 * 3`.
Новая команда: функция `c_xxx(int argc, char **argv)` и строка в таблице `table[]` в kcmds.c.

## Terminal в GUI

Окно-терминал: история (↑↓), правка строки (←→, Ctrl+A/E/U), Tab-дополнение, Ctrl+L/Ctrl+C, прокрутка колесом. Свои команды: `help clear exit apps run <app> echo`; остальное исполняет kshell (вместе с командами из kcmds.c) (`kshell_exec`), вывод `kprintf` перехватывается через `console_capture`.

## Display Test и частота экрана

GOP не сообщает и не меняет частоту обновления, а драйверов модесета GPU нет. «EDID Hz» на панели — максимум, который заявил монитор, «ui fps» — кадры композитора, реальный режим выставляет прошивка. Приложение Display Test показывает оба числа, бегущую полосу (наклон = тиринг) и экспериментальную пробу VGA-vblank (клавиша `P`).

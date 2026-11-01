# ROADMAP — детальный путь MigaOS до юзабельной ОС

План разбит на **микро-шаги**. Один шаг = одно небольшое изменение (обычно ≤ 300 строк, ≤ 5 файлов), после которого
проект собирается и загружается так же, как раньше. ОС растёт понемногу, а не большими кусками.

Как работать: читай `AGENTS.md`. Кратко: бери **первый свободный шаг**, ставь `WIP`, пиши код и запись в `docs/LOG.md`
(дата и время, файлы, самопроверка, команды для владельца), ставь `REVIEW`.
**Исполнитель код не компилирует и не запускает. Собирает и тестирует только владелец; `DONE` ставит только он.**

**Статусы:** `TODO` свободен · `WIP` в работе · `REVIEW` код написан (не компилировался), ждёт сборки и теста владельца · `DONE` проверен владельцем · `BLOCKED` блокер (причина в логе)

**Порядок выбора шага:** внутри задачи шаги идут строго по порядку (каждый зависит от предыдущего).
Задача доступна, когда все задачи из её строки «Зависит от» имеют все шаги в статусе `REVIEW` или `DONE`.

**🏁 Тест владельца** написан у каждой задачи: владелец проверяет задачу целиком, когда все её шаги в `REVIEW`
(не нужно тестировать каждый микро-шаг отдельно). После успеха он ставит `DONE` на все её шаги.

**Финальная цель («юзабельная ОС»):** грузится на реальном ПК/ноутбуке и в QEMU; запускает программы как отдельные процессы (user mode);
файлы на диске переживают перезагрузку; работают клавиатура и мышь (в т.ч. USB); GUI и приложения — отдельные ELF-программы;
есть shell и базовые утилиты; есть сеть (ping, wget); ОС ставится из образа.

**Если шаг оказался слишком большим:** разбей его на `N.N.Na`, `N.N.Nb` прямо в этом файле и сделай только первую часть.
**Нашёл новую проблему:** добавь новый шаг в конец нужной задачи (`N.N.<следующий номер>`), не меняя чужие.

## Волны (что можно делать параллельно)

| Волна | Задачи |
|---|---|
| **A** | 0.1 → 0.2 → 0.3; 0.4; 0.5; 0.6 |
| **B** | Этап 1 ‖ Этап 2 ‖ 4.1–4.2 ‖ 6.4–6.5 |
| **C** | Этап 3 ‖ 4.3–4.6 ‖ 6.1–6.3 |
| **D** | 4.7–4.10 ‖ 5.1 ‖ 7.1–7.3 |
| **E** | Этап 5 ‖ 7.4–7.7 ‖ 6.6–6.8 |
| **F** | Этап 8 |

---
# ЭТАП 0. Фундамент: запуск, тесты, отладка, документы

### 0.1 Первый запуск в QEMU — зависит от: —
🏁 *Тест владельца:* `make run` доходит до kshell/GUI, клавиатура работает; `make disk` даёт загружаемый образ.
README прямо говорит, что запуска ещё не было. Каждую найденную проблему оформляй отдельным шагом.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 0.1.1 | TODO | **Владелец** ставит зависимости, выполняет `make` и присылает полный вывод. Исполнитель разбирает вывод, составляет список ошибок/предупреждений и добавляет по шагу на каждую проблему (0.1.1a, 0.1.1b...). → список проблем в `docs/LOG.md` | — |
| 0.1.2 | TODO | **Владелец** запускает `make run` и присылает serial-лог (`-serial stdio`) и описание экрана. Исполнитель по логу определяет, где остановка (прошивка / MigaBoot / ядро), правок кода нет. → точка остановки описана | — |
| 0.1.3 | TODO | Починить первую проблему загрузчика, если она есть (GOP, чтение kernel.elf, карта памяти, ExitBootServices). Один фикс = один шаг; следующие добавлять как 0.1.3a, 0.1.3b. → MigaBoot передаёт управление ядру | `boot/uefi/` |
| 0.1.4 | TODO | Починить вход в ядро (`entry.S`, `_start`, приём `boot_info`, стек, BSS). → в serial появляется первое сообщение ядра | `kernel/arch/x86_64/`, `kernel/core/main.c` |
| 0.1.5 | TODO | Довести инициализацию до fbcon: GDT, IDT, PIC, paging, PMM без падений. → текст на экране, `mem` показывает память | `kernel/` |
| 0.1.6 | TODO | Довести до kshell: PIT, PS/2-клавиатура, `sti`. → команды `help`, `uptime`, `mem` работают | `drivers/`, `kernel/core/` |
| 0.1.7 | TODO | Довести до GUI: автозапуск, Terminal, мышь. Проверить `Ctrl+Q` и команду `gui`. → окно Terminal принимает ввод | `kernel/gui/` |
| 0.1.8 | TODO | Проверить `make disk` и загрузку `build/miga.img` в QEMU. → образ грузится | `Makefile`, `tools/mkimage.sh` |

### 0.2 Автотест `make test` — зависит от: 0.1
🏁 *Тест владельца:* `make test` печатает PASS и возвращает 0; если убрать маркер из ядра, возвращает ≠ 0.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 0.2.1 | TODO | Ядро печатает маркер `MIGA: BOOT OK` в serial после полной инициализации. → маркер виден в логе | `kernel/core/main.c` |
| 0.2.2 | TODO | Функция `qemu_exit(code)` через `isa-debug-exit` (порт 0xF4); команда kshell `qemu-exit N`. → QEMU завершается с кодом | `kernel/arch/x86_64/`, `kernel/core/kcmds.c` |
| 0.2.3 | TODO | Цель `make test`: QEMU без окна, `-serial file:build/serial.log`, `-device isa-debug-exit`, таймаут 30 с. → цель запускается | `Makefile` |
| 0.2.4 | TODO | `tools/runtest.sh`: ждёт маркер в логе, печатает PASS/FAIL, возвращает код. → PASS при нормальной загрузке | `tools/runtest.sh` |
| 0.2.5 | TODO | Негативная проверка: при отсутствии маркера скрипт даёт FAIL по таймауту. → подтверждено вручную и описано | `tools/runtest.sh`, `docs/` |

### 0.3 CI в GitHub Actions — зависит от: 0.2
🏁 *Тест владельца:* после push в GitHub вкладка Actions зелёная, бейдж в README отображается.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 0.3.1 | TODO | Workflow: установка пакетов + `make`. | `.github/workflows/ci.yml` |
| 0.3.2 | TODO | Добавить `make check`. | `.github/workflows/ci.yml` |
| 0.3.3 | TODO | Добавить `make test`. | `.github/workflows/ci.yml` |
| 0.3.4 | TODO | Сохранять артефактами `build/serial.log` и `build/esp/`. | `.github/workflows/ci.yml` |
| 0.3.5 | TODO | Бейдж статуса в `README.md`. | `README.md` |

### 0.4 Отладка и диагностика — зависит от: 0.1
🏁 *Тест владельца:* команда `crash` в kshell печатает регистры и читаемый стек вызовов с именами функций; `tools/gdb.sh` подключается к QEMU.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 0.4.1 | TODO | `tools/gdb.sh`: QEMU с `-s -S` + gdb, загрузка символов `kernel.elf`. → gdb останавливается на `_start` | `tools/gdb.sh` |
| 0.4.2 | TODO | Флаг `-fno-omit-frame-pointer` для ядра; проверить, что сборка не сломалась. | `Makefile` |
| 0.4.3 | TODO | Паника и исключения печатают все регистры, CR0/2/3/4, код ошибки. | `kernel/core/panic.c`, `kernel/arch/x86_64/idt.c` |
| 0.4.4 | TODO | Обход стека по RBP: печать адресов возврата в панике. | `kernel/core/panic.c` |
| 0.4.5 | TODO | Таблица символов: `nm` → `build/kernel.sym`; `tools/addr2line.sh` переводит адреса в имена. | `tools/`, `Makefile` |
| 0.4.6 | TODO | (Опционально) встроить символы в ядро, чтобы стек печатал имена функций сразу. | `kernel/core/` |

### 0.5 Документы-контракты — зависит от: 0.1
🏁 *Тест владельца:* файлы есть, читаются и соответствуют действительности.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 0.5.1 | TODO | `docs/CONTRACTS.md`: инвентаризация `include/miga/*.h`, что экспортирует ядро драйверам (6 функций из `make check`), правила изменения. | `docs/` |
| 0.5.2 | TODO | `docs/MEMORY.md`: текущая карта физической и виртуальной памяти (0x200000, сегменты, identity map). | `docs/` |
| 0.5.3 | TODO | `docs/ARCHITECTURE.md`: путь загрузки, стадии драйверов, потоки данных GUI. | `docs/` |
| 0.5.4 | TODO | `docs/ABI.md`: пустой скелет (разделы: регистры, номера syscall, ошибки, соглашения). | `docs/` |
| 0.5.5 | TODO | `docs/STYLE.md`: стиль кода по существующим файлам (отступы, имена, комментарии). | `docs/` |

### 0.6 Фреймворк тестов ядра (ktest) — зависит от: 0.2
🏁 *Тест владельца:* команда `ktest` в kshell выполняет тесты и печатает PASS/FAIL; `make test` запускает `ktest all`.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 0.6.1 | TODO | `ktest.h`: макросы `KTEST(name)`, `KASSERT(cond)`, регистрация в секцию `.ktests`. | `kernel/test/`, `kernel/include/kernel/ktest.h` |
| 0.6.2 | TODO | Запускалка тестов + команда kshell `ktest [name]`; тест-пример `ktest_selftest`. | `kernel/test/`, `kernel/core/kcmds.c` |
| 0.6.3 | TODO | Включить `ktest all` в `make test` (печатает итог и `MIGA: KTEST OK`). | `Makefile`, `tools/runtest.sh` |

---
# ЭТАП 1. Память

### 1.1 Куча ядра — зависит от: 0.1, 0.6
🏁 *Тест владельца:* `ktest heap` проходит; `heapinfo` показывает счётчики; после теста утечек нет.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 1.1.1 | TODO | `heap.h` API (`kmalloc kfree kzalloc krealloc`) + простейший bump-аллокатор на страницах PMM. | `kernel/mm/heap.c`, `kernel/include/kernel/heap.h` |
| 1.1.2 | TODO | Заменить bump на free-list с заголовками блоков и слиянием соседних свободных. | `kernel/mm/heap.c` |
| 1.1.3 | TODO | Выравнивание 16 байт, `krealloc`, `kzalloc`, защита от double free. | `kernel/mm/heap.c` |
| 1.1.4 | TODO | Режим отладки `HEAP_DEBUG`: канарейки по краям блока и заливка освобождённой памяти. | `kernel/mm/heap.c` |
| 1.1.5 | TODO | Команда `heapinfo`; `free`/`mem` показывают кучу. | `kernel/core/kcmds.c` |
| 1.1.6 | TODO | `ktest heap`: 100k аллокаций разных размеров, проверка паттернов, отсутствие утечек. | `kernel/test/` |
| 1.1.7 | TODO | (Опционально) slab-кэши для малых размеров. | `kernel/mm/slab.c` |

### 1.2 VMM — зависит от: 1.1
🏁 *Тест владельца:* `ktest vmm` проходит; обращение к снятой странице даёт понятное сообщение page fault.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 1.2.1 | TODO | `vmm.h`: API `vmm_map/unmap/protect/translate`, флаги страниц. | `kernel/include/kernel/vmm.h` |
| 1.2.2 | TODO | `vmm_translate`: обход таблиц страниц. | `kernel/mm/vmm.c` |
| 1.2.3 | TODO | `vmm_map` 4К с выделением промежуточных таблиц через PMM. | `kernel/mm/vmm.c` |
| 1.2.4 | TODO | `vmm_unmap` + `invlpg`, освобождение пустых таблиц. | `kernel/mm/vmm.c` |
| 1.2.5 | TODO | `vmm_protect` (R/W/X/User). | `kernel/mm/vmm.c` |
| 1.2.6 | TODO | Page fault handler: печать CR2, причины (P/W/U/I), панику с регистрами. | `kernel/arch/x86_64/idt.c` |
| 1.2.7 | TODO | Поддержка 2М-страниц в `vmm_map` (опц.). | `kernel/mm/vmm.c` |
| 1.2.8 | TODO | `ktest vmm`. | `kernel/test/` |

### 1.3 Higher-half ядро — зависит от: 1.2, 0.5
🏁 *Тест владельца:* ядро грузится и работает; `info` показывает высокие адреса; identity-отображение снято.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 1.3.1 | TODO | Спроектировать карту в `docs/MEMORY.md`: ядро `0xFFFFFFFF80000000`, direct map `0xFFFF800000000000`, зоны. | `docs/MEMORY.md` |
| 1.3.2 | TODO | Linker script: виртуальные адреса отдельно от физических (`AT()`), экспорт символов. | `kernel/kernel.ld` |
| 1.3.3 | TODO | Direct map всей физической памяти (по `boot_info`), функции `phys_to_virt/virt_to_phys`. | `kernel/mm/` |
| 1.3.4 | TODO | Вход в ядро: временные таблицы и прыжок на высокие адреса. | `kernel/arch/x86_64/entry.S`, `boot/uefi/` |
| 1.3.5 | TODO | Перевести PMM, fbcon, драйверы (MMIO) на direct map / `ioremap`. | `kernel/mm/`, `drivers/` |
| 1.3.6 | TODO | Снять identity mapping низких адресов. | `kernel/mm/paging.c` |
| 1.3.7 | TODO | `ktest` на корректность трансляции и отсутствие обращений по старым адресам. | `kernel/test/` |

### 1.4 Защита памяти — зависит от: 1.2
🏁 *Тест владельца:* искусственное переполнение стека ядра даёт панику с понятным сообщением; запись в `.text` даёт fault.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 1.4.1 | TODO | Включить NX (`EFER.NXE`), проставить NX на данные. | `kernel/arch/`, `kernel/mm/` |
| 1.4.2 | TODO | W^X-аудит: сегменты ядра R+X / R / R+W, тест на запись в `.text`. | `kernel/mm/`, `kernel/test/` |
| 1.4.3 | TODO | Guard-страницы под стеки ядра. | `kernel/mm/` |
| 1.4.4 | TODO | Освободить память `MEM_LOADER` в PMM после старта. | `kernel/mm/pmm.c` |
| 1.4.5 | TODO | SMEP/SMAP при наличии (CPUID). | `kernel/arch/x86_64/` |

---
# ЭТАП 2. Платформа: ACPI, APIC, время, питание

### 2.1 ACPI — зависит от: 0.1
🏁 *Тест владельца:* `lsacpi` в QEMU показывает таблицы, число CPU, IOAPIC и HPET.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 2.1.1 | TODO | `acpi.h`; поиск RSDP из `boot_info`, проверка контрольной суммы. | `kernel/acpi/` |
| 2.1.2 | TODO | Перебор XSDT/RSDT; команда `lsacpi` (сигнатуры, адреса). | `kernel/acpi/`, `kernel/core/kcmds.c` |
| 2.1.3 | TODO | MADT: LAPIC, IOAPIC, Interrupt Source Override. | `kernel/acpi/madt.c` |
| 2.1.4 | TODO | FADT: PM1a/b, reset register, флаги. | `kernel/acpi/fadt.c` |
| 2.1.5 | TODO | HPET и MCFG таблицы. | `kernel/acpi/` |

### 2.2 APIC — зависит от: 2.1
🏁 *Тест владельца:* таймер, клавиатура и мышь работают; `info` показывает режим «APIC».

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 2.2.1 | TODO | Определение LAPIC (CPUID/MSR), включение, spurious vector. | `kernel/arch/x86_64/apic.c` |
| 2.2.2 | TODO | Маскировка PIC (ремап и отключение). | `kernel/arch/x86_64/pic.c` |
| 2.2.3 | TODO | Инициализация IOAPIC, redirection table. | `kernel/arch/x86_64/ioapic.c` |
| 2.2.4 | TODO | Учёт ISO (например IRQ0→GSI2). | `kernel/arch/x86_64/ioapic.c` |
| 2.2.5 | TODO | `irq.c`: регистрация обработчиков по GSI; EOI через LAPIC. | `kernel/core/irq.c`, `include/miga/irq.h` |
| 2.2.6 | TODO | Клавиатура IRQ1 через IOAPIC. | `drivers/input/ps2kbd.c` |
| 2.2.7 | TODO | Мышь IRQ12 через IOAPIC. | `drivers/input/ps2mouse.c` |

### 2.3 TSS и IST — зависит от: 0.1
🏁 *Тест владельца:* тестовая команда с переполнением стека даёт обработанный double fault, а не перезагрузку.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 2.3.1 | TODO | Структура TSS, дескриптор в GDT. | `kernel/arch/x86_64/gdt.c` |
| 2.3.2 | TODO | `ltr`; RSP0 для ring 3→0. | `kernel/arch/x86_64/gdt.c` |
| 2.3.3 | TODO | Стеки IST для #DF, NMI, #MC. | `kernel/arch/x86_64/` |
| 2.3.4 | TODO | Поле IST в записях IDT. | `kernel/arch/x86_64/idt.c` |
| 2.3.5 | TODO | Команда `crash stack` для проверки. | `kernel/core/kcmds.c` |

### 2.4 Время — зависит от: 2.2
🏁 *Тест владельца:* `uptime` и `date` корректны; PIT не используется.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 2.4.1 | TODO | Калибровка TSC (по PIT на старте), проверка invariant TSC. | `kernel/core/time.c` |
| 2.4.2 | TODO | Калибровка LAPIC-таймера по TSC. | `kernel/arch/x86_64/apic.c` |
| 2.4.3 | TODO | Периодические тики от LAPIC (100 Гц); PIT отключается. | `kernel/core/time.c`, `drivers/timer/` |
| 2.4.4 | TODO | RTC: чтение даты/времени, `date`. | `drivers/timer/rtc.c`, `kernel/core/kcmds.c` |
| 2.4.5 | TODO | (Опц.) HPET как запасной источник. | `drivers/timer/hpet.c` |

### 2.5 Питание — зависит от: 2.1
🏁 *Тест владельца:* `halt` выключает, `reboot` перезагружает QEMU.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 2.5.1 | TODO | Перезагрузка: reset register FADT, запасные 0xCF9 и KBC. | `kernel/acpi/power.c` |
| 2.5.2 | TODO | Выключение QEMU (порт 0x604 / ACPI PM1a). | `kernel/acpi/power.c` |
| 2.5.3 | TODO | Минимальный разбор DSDT для `_S5` (реальное железо). | `kernel/acpi/` |
| 2.5.4 | TODO | Подключить к командам `halt`/`reboot`. | `kernel/core/kcmds.c` |

---
# ЭТАП 3. Процессы и user mode (критический рубеж)

### 3.1 Задачи и переключение контекста — зависит от: 1.1, 2.3
🏁 *Тест владельца:* `ktest threads` — два потока по очереди печатают, система не падает.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 3.1.1 | TODO | `struct task` (id, состояние, kernel stack, контекст), глобальный список. | `kernel/sched/task.c`, `kernel/include/kernel/task.h` |
| 3.1.2 | TODO | Выделение kernel stack (с guard-страницей). | `kernel/sched/task.c` |
| 3.1.3 | TODO | `switch_to` на ассемблере (callee-saved регистры, RSP). | `kernel/arch/x86_64/switch.S` |
| 3.1.4 | TODO | `kthread_create(fn, arg)`, трамплин запуска. | `kernel/sched/task.c` |
| 3.1.5 | TODO | Idle-задача. | `kernel/sched/` |
| 3.1.6 | TODO | `ktest threads`. | `kernel/test/` |

### 3.2 Планировщик и синхронизация — зависит от: 3.1, 2.4
🏁 *Тест владельца:* `ktest sched` (N потоков, общий счётчик) даёт точный результат; kshell остаётся отзывчивым.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 3.2.1 | TODO | Очередь готовых задач, `schedule()`, `yield()`. | `kernel/sched/sched.c` |
| 3.2.2 | TODO | Вытесняющий режим по таймеру. | `kernel/sched/sched.c`, `kernel/core/time.c` |
| 3.2.3 | TODO | Spinlock с сохранением флагов прерываний. | `kernel/include/kernel/spinlock.h` |
| 3.2.4 | TODO | Очереди ожидания (`wait_on`, `wake_up`). | `kernel/sched/wait.c` |
| 3.2.5 | TODO | `sleep_ms` через таймер. | `kernel/sched/` |
| 3.2.6 | TODO | Mutex и семафор. | `kernel/sched/sync.c` |
| 3.2.7 | TODO | `ktest sched`. | `kernel/test/` |
| 3.2.8 | TODO | (Опц.) приоритеты задач. | `kernel/sched/` |

### 3.3 Процессы и адресные пространства — зависит от: 1.3, 3.1
🏁 *Тест владельца:* встроенная тестовая программа выполняется в ring 3 и печатает строку (через временный syscall).

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 3.3.1 | TODO | `struct proc`, привязка к task, поля mm. | `kernel/proc/proc.c` |
| 3.3.2 | TODO | Создание PML4 процесса с копией ядерной половины. | `kernel/mm/`, `kernel/proc/` |
| 3.3.3 | TODO | Смена CR3 при переключении задач. | `kernel/sched/` |
| 3.3.4 | TODO | GDT: сегменты user code/data. | `kernel/arch/x86_64/gdt.c` |
| 3.3.5 | TODO | Парсер ELF64 для user, отображение PT_LOAD. | `kernel/proc/elf.c` |
| 3.3.6 | TODO | User stack, начальное состояние (argc/argv). | `kernel/proc/` |
| 3.3.7 | TODO | Переход в ring 3 (`iretq`). | `kernel/arch/x86_64/` |
| 3.3.8 | TODO | Встроенный тестовый user-ELF (в массиве) и запуск командой `runtest`. | `kernel/proc/`, `tools/` |

### 3.4 Syscalls — зависит от: 3.3, 0.5
🏁 *Тест владельца:* user-программа делает `write` и `exit`; обращение к плохому указателю убивает процесс, а ядро живёт.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 3.4.1 | TODO | MSR `STAR/LSTAR/SFMASK`, включение `SYSCALL`. | `kernel/arch/x86_64/` |
| 3.4.2 | TODO | Вход `syscall_entry` (swapgs, стек ядра), возврат `sysret`. | `kernel/arch/x86_64/syscall.S` |
| 3.4.3 | TODO | Таблица обработчиков, коды ошибок. | `kernel/syscall/` |
| 3.4.4 | TODO | `copy_from_user`/`copy_to_user` с проверкой диапазона и обработкой fault. | `kernel/syscall/uaccess.c` |
| 3.4.5 | TODO | `write(fd=1/2)` в консоль, `exit`. | `kernel/syscall/` |
| 3.4.6 | TODO | `yield`, `getpid`, `sleep_ms`. | `kernel/syscall/` |
| 3.4.7 | TODO | `mmap/munmap/brk` (анонимная память). | `kernel/syscall/`, `kernel/mm/` |
| 3.4.8 | TODO | Fault в user-режиме → завершение процесса с сообщением. | `kernel/arch/x86_64/idt.c` |
| 3.4.9 | TODO | `docs/ABI.md` v0: номера, регистры, ошибки. | `docs/ABI.md` |

### 3.5 IPC — зависит от: 3.2, 3.4
🏁 *Тест владельца:* два процесса обмениваются сообщениями через pipe и очередь.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 3.5.1 | TODO | Кольцевой буфер ядра с блокировкой. | `kernel/ipc/ring.c` |
| 3.5.2 | TODO | Pipe: `pipe()`, блокирующие `read/write`. | `kernel/ipc/pipe.c` |
| 3.5.3 | TODO | Очереди сообщений (`mq_send/mq_recv`). | `kernel/ipc/mq.c` |
| 3.5.4 | TODO | Shared memory: `shm_create/shm_map`. | `kernel/ipc/shm.c` |
| 3.5.5 | TODO | `poll`/ожидание нескольких источников. | `kernel/ipc/poll.c` |

### 3.6 Жизненный цикл процесса — зависит от: 3.4
🏁 *Тест владельца:* `ktest proc` (spawn/exit ×1000) без утечек памяти; `ps` показывает процессы.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 3.6.1 | TODO | `spawn(path, argv)` (пока из встроенных ELF). | `kernel/proc/` |
| 3.6.2 | TODO | `wait`, зомби-процессы, код возврата. | `kernel/proc/` |
| 3.6.3 | TODO | Освобождение адресного пространства и ресурсов при выходе. | `kernel/proc/`, `kernel/mm/` |
| 3.6.4 | TODO | `kill` и простые сигналы. | `kernel/proc/` |
| 3.6.5 | TODO | Команда `ps` в kshell. | `kernel/core/kcmds.c` |
| 3.6.6 | TODO | `ktest proc`: 1000 циклов, проверка счётчиков PMM. | `kernel/test/` |

---
# ЭТАП 4. Хранилище и файловые системы

### 4.1 PCI — зависит от: 2.2, 2.1
🏁 *Тест владельца:* `lspci` в QEMU показывает устройства с vendor/device и BAR.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.1.1 | TODO | `include/miga/pci.h` API; чтение/запись конфигурации через 0xCF8/0xCFC. | `include/miga/pci.h`, `drivers/bus/pci.c` |
| 4.1.2 | TODO | Рекурсивный перебор шин, мостов, функций. | `drivers/bus/pci.c` |
| 4.1.3 | TODO | Команда `lspci`. | `kernel/core/kcmds.c` |
| 4.1.4 | TODO | Разбор BAR (тип, размер) и включение bus mastering. | `drivers/bus/pci.c` |
| 4.1.5 | TODO | Список capabilities; MSI/MSI-X. | `drivers/bus/pci.c` |
| 4.1.6 | TODO | ECAM через MCFG (PCIe). | `drivers/bus/pci.c` |
| 4.1.7 | TODO | Привязка драйверов по class/vendor/device в `MIGA_DRIVER`. | `include/miga/driver.h` |

### 4.2 Блочный слой — зависит от: 1.1
🏁 *Тест владельца:* `lsblk` показывает RAM-диск; `ktest block` проходит.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.2.1 | TODO | `include/miga/block.h`: структура устройства, `read/write` секторов. | `include/miga/block.h` |
| 4.2.2 | TODO | Реестр блочных устройств, команда `lsblk`. | `kernel/block/` |
| 4.2.3 | TODO | RAM-диск для тестов. | `drivers/block/ramdisk.c` |
| 4.2.4 | TODO | Кэш блоков (LRU, write-back) и `flush`. | `kernel/block/cache.c` |
| 4.2.5 | TODO | `ktest block`. | `kernel/test/` |

### 4.3 virtio-blk — зависит от: 4.1, 4.2
🏁 *Тест владельца:* `make run` с диском; `lsblk` показывает диск; чтение и запись секторов работают.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.3.1 | TODO | Общая часть virtio-PCI (capabilities, negotiation). | `drivers/virtio/virtio_pci.c` |
| 4.3.2 | TODO | Split virtqueue (descriptor/avail/used). | `drivers/virtio/virtqueue.c` |
| 4.3.3 | TODO | Инициализация virtio-blk, ёмкость. | `drivers/block/virtio_blk.c` |
| 4.3.4 | TODO | Чтение секторов (polling). | `drivers/block/virtio_blk.c` |
| 4.3.5 | TODO | Запись и flush. | `drivers/block/virtio_blk.c` |
| 4.3.6 | TODO | Режим на прерываниях. | `drivers/block/virtio_blk.c` |
| 4.3.7 | TODO | Цель `make run-disk` с подключением `build/miga.img` как virtio-диска. | `Makefile` |

### 4.4 AHCI — зависит от: 4.1, 4.2
🏁 *Тест владельца:* в QEMU `-device ahci` диск виден и читается.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.4.1 | TODO | Обнаружение контроллера, ABAR, включение AHCI-режима. | `drivers/block/ahci.c` |
| 4.4.2 | TODO | Инициализация портов, command list, FIS. | `drivers/block/ahci.c` |
| 4.4.3 | TODO | `IDENTIFY`. | `drivers/block/ahci.c` |
| 4.4.4 | TODO | `READ DMA EXT`. | `drivers/block/ahci.c` |
| 4.4.5 | TODO | `WRITE DMA EXT`, flush. | `drivers/block/ahci.c` |
| 4.4.6 | TODO | Прерывания. | `drivers/block/ahci.c` |

### 4.5 NVMe — зависит от: 4.1, 4.2
🏁 *Тест владельца:* в QEMU `-device nvme` диск виден и читается.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.5.1 | TODO | BAR, CAP, включение контроллера. | `drivers/block/nvme.c` |
| 4.5.2 | TODO | Admin queue. | `drivers/block/nvme.c` |
| 4.5.3 | TODO | Identify controller/namespace. | `drivers/block/nvme.c` |
| 4.5.4 | TODO | I/O queue pair. | `drivers/block/nvme.c` |
| 4.5.5 | TODO | Чтение и запись. | `drivers/block/nvme.c` |
| 4.5.6 | TODO | Прерывания (MSI-X). | `drivers/block/nvme.c` |

### 4.6 GPT — зависит от: 4.2
🏁 *Тест владельца:* `lsblk` показывает разделы тестового образа.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.6.1 | TODO | CRC32 и чтение заголовка GPT. | `kernel/block/gpt.c` |
| 4.6.2 | TODO | Чтение записей разделов, GUID типов. | `kernel/block/gpt.c` |
| 4.6.3 | TODO | Регистрация разделов как блочных устройств. | `kernel/block/gpt.c` |
| 4.6.4 | TODO | Запасной разбор MBR. | `kernel/block/mbr.c` |

### 4.7 Настоящий VFS — зависит от: 3.4, 4.2
🏁 *Тест владельца:* в kshell `ls`, `cat`, `mkdir` работают на тестовой ФС; user-программа читает файл через `open/read`.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.7.1 | TODO | `vnode` и `fs_ops` (интерфейс драйвера ФС). | `kernel/include/kernel/vfs.h` |
| 4.7.2 | TODO | Разбор пути, `vfs_resolve()` с учётом точек монтирования (совместимо с текущим). | `kernel/core/vfs.c` |
| 4.7.3 | TODO | ramfs (временная ФС для тестов VFS). | `kernel/fs/ramfs.c` |
| 4.7.4 | TODO | Таблица fd процесса; `open/close/read/write/lseek`. | `kernel/core/vfs.c`, `kernel/proc/` |
| 4.7.5 | TODO | `stat`, `readdir`. | `kernel/core/vfs.c` |
| 4.7.6 | TODO | `mkdir/unlink/rename`. | `kernel/core/vfs.c` |
| 4.7.7 | TODO | Syscalls для файлов; cwd процесса. | `kernel/syscall/` |
| 4.7.8 | TODO | Команды kshell `ls cat mkdir rm` на настоящем VFS. | `kernel/core/kcmds.c` |

### 4.8 initrd / tarfs — зависит от: 4.7
🏁 *Тест владельца:* после загрузки `ls /` и `cat /etc/motd` работают из initrd.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.8.1 | TODO | Парсер tar (ustar), tarfs только на чтение. | `kernel/fs/tarfs.c` |
| 4.8.2 | TODO | MigaBoot загружает `\EFI\MIGA\initrd.tar` и передаёт в `boot_info`. | `boot/uefi/main.c`, `include/miga/boot_info.h` |
| 4.8.3 | TODO | Монтирование tarfs как `/`. | `kernel/core/vfs.c` |
| 4.8.4 | TODO | `tools/mkinitrd.sh` + цель `make initrd`. | `tools/`, `Makefile` |

### 4.9 FAT32 — зависит от: 4.7, 4.6
🏁 *Тест владельца:* файл, созданный в Миге, виден на хосте (`mtools`), и наоборот.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.9.1 | TODO | Разбор BPB, определение FAT32. | `kernel/fs/fat32.c` |
| 4.9.2 | TODO | Чтение цепочек FAT. | `kernel/fs/fat32.c` |
| 4.9.3 | TODO | Каталоги, короткие имена и LFN (чтение). | `kernel/fs/fat32.c` |
| 4.9.4 | TODO | Чтение файлов. | `kernel/fs/fat32.c` |
| 4.9.5 | TODO | Запись, расширение цепочек. | `kernel/fs/fat32.c` |
| 4.9.6 | TODO | Создание и удаление файлов/каталогов. | `kernel/fs/fat32.c` |
| 4.9.7 | TODO | Монтирование ESP как `/boot` (сейчас только запись в таблице). | `kernel/core/vfs.c` |

### 4.10 Корневая ФС (ext2 или своя) — зависит от: 4.7
🏁 *Тест владельца:* корень на диске: права, время, переживает перезагрузку.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 4.10.1 | TODO | Выбор ФС (рекомендация: ext2) и запись решения в `docs/`. | `docs/` |
| 4.10.2 | TODO | Суперблок, дескрипторы групп. | `kernel/fs/ext2.c` |
| 4.10.3 | TODO | Чтение inode и каталогов. | `kernel/fs/ext2.c` |
| 4.10.4 | TODO | Чтение файлов (прямые и косвенные блоки). | `kernel/fs/ext2.c` |
| 4.10.5 | TODO | Выделение блоков и inode. | `kernel/fs/ext2.c` |
| 4.10.6 | TODO | Запись, создание, удаление. | `kernel/fs/ext2.c` |
| 4.10.7 | TODO | Права и время файлов (uid/gid/mode/mtime). | `kernel/fs/ext2.c` |
| 4.10.8 | TODO | Хостовая сборка образа (`mke2fs`/скрипт) для `make image`. | `tools/` |

---
# ЭТАП 5. User space и ABI

### 5.1 Кросс-тулчейн `x86_64-miga` — зависит от: 3.4
🏁 *Тест владельца:* `x86_64-miga-gcc hello.c -o hello` создаёт ELF, который запускается в Миге.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 5.1.1 | TODO | Структура sysroot, целевая тройка, список патчей (`docs/TOOLCHAIN.md`). | `docs/`, `tools/toolchain/` |
| 5.1.2 | TODO | Скрипт сборки binutils для `x86_64-miga` (допустимо использовать `x86_64-elf` как временный вариант). | `tools/toolchain/` |
| 5.1.3 | TODO | Скрипт сборки gcc (stage 1, без libc). | `tools/toolchain/` |
| 5.1.4 | TODO | Линкер-скрипт и `crt0` для user-программ. | `user/` |
| 5.1.5 | TODO | Freestanding hello (без libc) запускается через `spawn`. | `user/hello/` |

### 5.2 libc — зависит от: 5.1
🏁 *Тест владельца:* user-программа с `printf`, `malloc`, `fopen` работает.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 5.2.1 | TODO | Обёртки syscall, `_start`, `exit`. | `user/libc/` |
| 5.2.2 | TODO | `string.h` / `memcpy` / `memset`. | `user/libc/` |
| 5.2.3 | TODO | `malloc/free` поверх `brk/mmap`. | `user/libc/` |
| 5.2.4 | TODO | `printf` семейство, `puts`. | `user/libc/` |
| 5.2.5 | TODO | `fopen/fread/fwrite/fclose`. | `user/libc/` |
| 5.2.6 | TODO | `stdlib`, `ctype`, `time`. | `user/libc/` |
| 5.2.7 | TODO | (Альтернатива) порт musl вместо своей libc; решение записать в `docs/`. | `user/libc/` |

### 5.3 init и user-shell — зависит от: 5.2, 4.8
🏁 *Тест владельца:* после загрузки запускается shell в user mode; можно запустить программу по имени.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 5.3.1 | TODO | `/dev/console`: чтение клавиатуры и вывод на консоль через VFS. | `kernel/dev/`, `kernel/core/vfs.c` |
| 5.3.2 | TODO | Ядро запускает `/sbin/init` из initrd. | `kernel/core/main.c` |
| 5.3.3 | TODO | `init` запускает shell. | `user/init/` |
| 5.3.4 | TODO | Shell: ввод строки, разбор, `spawn` + `wait`. | `user/sh/` |
| 5.3.5 | TODO | Встроенные `cd`, `exit`, `export`. | `user/sh/` |
| 5.3.6 | TODO | Пайпы `|` и перенаправления `> <`. | `user/sh/` |
| 5.3.7 | TODO | `PATH`, история, редактирование строки. | `user/sh/` |

### 5.4 Базовые утилиты — зависит от: 5.3, 4.7
🏁 *Тест владельца:* каждая утилита работает на файлах диска.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 5.4.1 | TODO | `ls`, `cat`. | `user/bin/` |
| 5.4.2 | TODO | `cp`, `mv`, `rm`. | `user/bin/` |
| 5.4.3 | TODO | `mkdir`, `rmdir`, `touch`, `echo`, `pwd`. | `user/bin/` |
| 5.4.4 | TODO | `ps`, `kill`, `free`, `uname`. | `user/bin/` |
| 5.4.5 | TODO | `head`, `tail`, `wc`, `grep` (простой). | `user/bin/` |
| 5.4.6 | TODO | `mount`, `umount`, `df`, `lsblk`. | `user/bin/` |

### 5.5 GUI-сервер в user space — зависит от: 3.5, 5.2
🏁 *Тест владельца:* GUI работает как отдельный процесс; ядро больше не содержит композитор.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 5.5.1 | TODO | Syscall/устройство `/dev/fb0`: отображение фреймбуфера (write-combining) в процесс. | `kernel/dev/`, `kernel/mm/` |
| 5.5.2 | TODO | `/dev/input`: события клавиатуры и мыши для user space. | `kernel/dev/`, `include/miga/input.h` |
| 5.5.3 | TODO | Клиентская библиотека `libgui` поверх IPC (API как `gui.h`). | `user/libgui/` |
| 5.5.4 | TODO | Перенос логики сервера (окна, z-порядок, фокус) в процесс `guisrv`. | `user/guisrv/` |
| 5.5.5 | TODO | Общие поверхности окон через shared memory. | `user/guisrv/`, `kernel/ipc/shm.c` |
| 5.5.6 | TODO | Композитор с dirty-rect и счётчиком fps в user space. | `user/guisrv/` |
| 5.5.7 | TODO | Удалить серверный код из ядра, оставить только kshell. | `kernel/gui/` |

### 5.6 GUI-приложения как ELF — зависит от: 5.5
🏁 *Тест владельца:* каждое приложение запускается из меню, падение приложения закрывает только его окно.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 5.6.1 | TODO | Terminal (теперь запускает user-shell). | `user/apps/terminal/` |
| 5.6.2 | TODO | Notepad (с сохранением файлов). | `user/apps/notepad/` |
| 5.6.3 | TODO | Calc, Sysmon. | `user/apps/` |
| 5.6.4 | TODO | Files, Settings (на файлах и `/etc`). | `user/apps/` |
| 5.6.5 | TODO | Devices, Display Test. | `user/apps/` |
| 5.6.6 | TODO | Snake, Paint, Mines. | `user/apps/` |

---
# ЭТАП 6. Ввод и железо

### 6.1 xHCI (USB-контроллер) — зависит от: 4.1
🏁 *Тест владельца:* QEMU `-device qemu-xhci -device usb-kbd` определяет устройство.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 6.1.1 | TODO | Аудит существующего кода USB в `drivers/` (README упоминает `usbhid`/`xhci`); результат в лог. | `drivers/usb/` |
| 6.1.2 | TODO | Сброс и запуск контроллера, DCBAA. | `drivers/usb/xhci.c` |
| 6.1.3 | TODO | Command ring и event ring. | `drivers/usb/xhci.c` |
| 6.1.4 | TODO | Enable Slot, Address Device, control-передачи. | `drivers/usb/xhci.c` |
| 6.1.5 | TODO | Чтение дескрипторов, `lsusb`. | `drivers/usb/`, `kernel/core/kcmds.c` |
| 6.1.6 | TODO | Interrupt-передачи. | `drivers/usb/xhci.c` |

### 6.2 USB-хабы — зависит от: 6.1
🏁 *Тест владельца:* устройство через хаб (в QEMU `usb-hub`) определяется.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 6.2.1 | TODO | Дескриптор хаба, питание портов. | `drivers/usb/hub.c` |
| 6.2.2 | TODO | Сброс порта, определение скорости. | `drivers/usb/hub.c` |
| 6.2.3 | TODO | Перечисление дочерних устройств. | `drivers/usb/hub.c` |
| 6.2.4 | TODO | Горячее подключение (изменение статуса). | `drivers/usb/hub.c` |

### 6.3 USB HID — зависит от: 6.1
🏁 *Тест владельца:* USB-клавиатура и мышь работают без PS/2-эмуляции.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 6.3.1 | TODO | Boot-клавиатура → события `input.h`. | `drivers/input/usbkbd.c` |
| 6.3.2 | TODO | Boot-мышь → события (двигается курсор). | `drivers/input/usbmouse.c` |
| 6.3.3 | TODO | Парсер report descriptor. | `drivers/input/hid.c` |
| 6.3.4 | TODO | Повтор клавиш, LED. | `drivers/input/` |

### 6.4 Раскладки и UTF-8 — зависит от: 0.1
🏁 *Тест владельца:* в Terminal набирается русский текст, переключение раскладки работает.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 6.4.1 | TODO | Таблицы раскладок (формат + EN). | `drivers/input/keymap.c` |
| 6.4.2 | TODO | Раскладка RU. | `drivers/input/keymap.c` |
| 6.4.3 | TODO | Переключение (Alt+Shift или Ctrl+Space), индикатор. | `drivers/input/`, `kernel/gui/` |
| 6.4.4 | TODO | UTF-8 в `input.h` (события с кодпоинтом). | `include/miga/input.h` |

### 6.5 Unicode-шрифт — зависит от: 0.1
🏁 *Тест владельца:* русский текст отображается в kshell, Terminal и Notepad.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 6.5.1 | TODO | `genfont.py`: добавить кириллицу (U+0400–04FF) в bitmap 8x16. | `tools/genfont.py` |
| 6.5.2 | TODO | Декодирование UTF-8 в fbcon. | `drivers/video/fbcon/` |
| 6.5.3 | TODO | Декодирование UTF-8 в GUI-текст. | `kernel/gui/gfx.c` |
| 6.5.4 | TODO | (Опц.) TTF-рендер (stb_truetype) и размеры шрифта. | `kernel/gui/` |

### 6.6 I2C-HID тачпады ноутбуков — зависит от: 6.1, 2.2 (сложно)
🏁 *Тест владельца:* тачпад на реальном ноутбуке двигает курсор.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 6.6.1 | TODO | Минимальный разбор AML (имена, `_HID`, `_CRS`). | `kernel/acpi/aml.c` |
| 6.6.2 | TODO | Драйвер I2C-контроллера (Intel LPSS / DesignWare). | `drivers/i2c/` |
| 6.6.3 | TODO | HID over I2C. | `drivers/input/i2chid.c` |
| 6.6.4 | TODO | Прерывания по GPIO. | `drivers/gpio/` |

### 6.7 virtio-gpu (опционально) — зависит от: 4.1
🏁 *Тест владельца:* смена разрешения в QEMU.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 6.7.1 | TODO | Инициализация и получение режимов. | `drivers/video/virtio_gpu.c` |
| 6.7.2 | TODO | 2D-ресурс, scanout, flush. | `drivers/video/virtio_gpu.c` |
| 6.7.3 | TODO | Смена разрешения на лету. | `drivers/video/virtio_gpu.c` |

### 6.8 Проверка на реальном железе — зависит от: 4.3, 6.3
🏁 *Тест владельца:* ОС загрузилась хотя бы на одной реальной машине, результат записан.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 6.8.1 | TODO | Шаблон `docs/HARDWARE.md` (модель, прошивка, что работает/нет, serial-лог). | `docs/HARDWARE.md` |
| 6.8.2 | TODO | Инструкция записи образа на USB и настройки UEFI (Secure Boot выкл). | `docs/HARDWARE.md` |
| 6.8.3 | TODO | Первая реальная машина (**заполняет владелец**). | `docs/HARDWARE.md` |

---
# ЭТАП 7. Сеть

### 7.1 Сетевой интерфейс и virtio-net — зависит от: 4.1
🏁 *Тест владельца:* в QEMU `-device virtio-net` кадры уходят и приходят (счётчики в `ifconfig`).

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 7.1.1 | TODO | `include/miga/net.h`: структура интерфейса, tx/rx. | `include/miga/net.h` |
| 7.1.2 | TODO | virtio-net: инициализация, MAC. | `drivers/net/virtio_net.c` |
| 7.1.3 | TODO | Отправка кадров. | `drivers/net/virtio_net.c` |
| 7.1.4 | TODO | Приём кадров, прерывания. | `drivers/net/virtio_net.c` |
| 7.1.5 | TODO | Команда `ifconfig`. | `kernel/core/kcmds.c` |

### 7.2 Intel e1000 — зависит от: 7.1
🏁 *Тест владельца:* QEMU `-device e1000` работает как virtio-net.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 7.2.1 | TODO | Инициализация, MAC из EEPROM. | `drivers/net/e1000.c` |
| 7.2.2 | TODO | TX-кольцо. | `drivers/net/e1000.c` |
| 7.2.3 | TODO | RX-кольцо, прерывания. | `drivers/net/e1000.c` |

### 7.3 Realtek rtl8169 — зависит от: 7.1
🏁 *Тест владельца:* QEMU `-device rtl8139`/реальная карта работает (или помечено в `HARDWARE.md`).

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 7.3.1 | TODO | Инициализация. | `drivers/net/rtl8169.c` |
| 7.3.2 | TODO | TX. | `drivers/net/rtl8169.c` |
| 7.3.3 | TODO | RX, прерывания. | `drivers/net/rtl8169.c` |

### 7.4 Сетевой стек — зависит от: 7.1, 1.1
🏁 *Тест владельца:* внешний `ping` до ядра отвечает; ядро отвечает на TCP-подключение к тестовому порту.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 7.4.1 | TODO | Решение: свой стек или порт lwIP (запись в `docs/`). | `docs/` |
| 7.4.2 | TODO | Буферы пакетов, Ethernet. | `kernel/net/` |
| 7.4.3 | TODO | ARP. | `kernel/net/arp.c` |
| 7.4.4 | TODO | IPv4. | `kernel/net/ip.c` |
| 7.4.5 | TODO | ICMP echo. | `kernel/net/icmp.c` |
| 7.4.6 | TODO | UDP. | `kernel/net/udp.c` |
| 7.4.7 | TODO | TCP: состояния, рукопожатие. | `kernel/net/tcp.c` |
| 7.4.8 | TODO | TCP: передача данных, окно, повторы. | `kernel/net/tcp.c` |

### 7.5 Сокеты — зависит от: 7.4, 3.4
🏁 *Тест владельца:* user-программа открывает TCP-соединение к хосту.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 7.5.1 | TODO | `socket/bind/listen/accept`. | `kernel/net/socket.c` |
| 7.5.2 | TODO | `connect/send/recv`. | `kernel/net/socket.c` |
| 7.5.3 | TODO | Сокеты как fd в VFS, `poll`. | `kernel/net/`, `kernel/core/vfs.c` |
| 7.5.4 | TODO | Обновить `docs/ABI.md`. | `docs/ABI.md` |

### 7.6 DHCP и DNS — зависит от: 7.5
🏁 *Тест владельца:* адрес получается автоматически, имена резолвятся.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 7.6.1 | TODO | DHCP-клиент. | `user/net/dhcp.c` |
| 7.6.2 | TODO | DNS-клиент. | `user/net/dns.c` |
| 7.6.3 | TODO | `/etc/resolv.conf`, настройка маршрутов. | `user/net/` |

### 7.7 Сетевые утилиты — зависит от: 7.6, 5.4
🏁 *Тест владельца:* `wget http://...` скачивает файл на диск.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 7.7.1 | TODO | `ping`. | `user/bin/` |
| 7.7.2 | TODO | `nslookup`. | `user/bin/` |
| 7.7.3 | TODO | `wget` (HTTP). | `user/bin/` |

---
# ЭТАП 8. Релиз и доводка

### 8.1 Сборка образов — зависит от: 4.9, 5.3
🏁 *Тест владельца:* `make image` даёт `miga.img` и ISO, оба грузятся в QEMU; образ грузится с USB на реальной машине.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 8.1.1 | TODO | `make image`: GPT + ESP + корневой раздел. | `tools/mkimage.sh`, `Makefile` |
| 8.1.2 | TODO | ISO с UEFI-загрузкой. | `tools/mkiso.sh` |
| 8.1.3 | TODO | Публикация образов артефактами CI. | `.github/workflows/` |

### 8.2 Установщик — зависит от: 8.1, 4.10
🏁 *Тест владельца:* установка на виртуальный диск, затем загрузка с него.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 8.2.1 | TODO | Выбор диска (`lsblk`-подобный список). | `user/installer/` |
| 8.2.2 | TODO | Разметка GPT и форматирование. | `user/installer/` |
| 8.2.3 | TODO | Копирование системы и установка MigaBoot в ESP. | `user/installer/` |
| 8.2.4 | TODO | GUI-версия установщика (опц.). | `user/installer/` |

### 8.3 Порты ПО — зависит от: 5.2, 5.4
🏁 *Тест владельца:* каждый порт запускается и выполняет базовую задачу.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 8.3.1 | TODO | Lua. | `ports/lua/` |
| 8.3.2 | TODO | Текстовый редактор (kilo или аналог). | `ports/` |
| 8.3.3 | TODO | tcc (компилятор C внутри Миги). | `ports/tcc/` |
| 8.3.4 | TODO | DOOM (doomgeneric) через `libgui`. | `ports/doom/` |

### 8.4 Пользователи и права — зависит от: 4.10, 3.6
🏁 *Тест владельца:* вход по паролю, обычный пользователь не может читать root-файлы.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 8.4.1 | TODO | uid/gid в процессе, syscalls `getuid/setuid`. | `kernel/proc/` |
| 8.4.2 | TODO | Проверка прав в VFS. | `kernel/core/vfs.c` |
| 8.4.3 | TODO | `/etc/passwd`, хеши паролей. | `user/` |
| 8.4.4 | TODO | `login`, `su`, `id`. | `user/bin/` |

### 8.5 SMP — зависит от: 2.2, 3.2
🏁 *Тест владельца:* `lscpu` показывает все ядра; `ktest smp` проходит.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 8.5.1 | TODO | Per-CPU данные (через GS). | `kernel/arch/x86_64/` |
| 8.5.2 | TODO | Запуск AP (INIT-SIPI, трамплин). | `kernel/arch/x86_64/` |
| 8.5.3 | TODO | Планировщик на N ядер, per-CPU очереди. | `kernel/sched/` |
| 8.5.4 | TODO | TLB shootdown (IPI). | `kernel/mm/` |
| 8.5.5 | TODO | Аудит блокировок, `ktest smp`. | `kernel/` |

### 8.6 Документация — зависит от: всё
🏁 *Тест владельца:* по гайду можно написать и запустить своё приложение.

| ID | Статус | Шаг → критерий готовности | Пути |
|---|---|---|---|
| 8.6.1 | TODO | Полный `docs/ABI.md`. | `docs/` |
| 8.6.2 | TODO | `docs/ARCHITECTURE.md` (актуализация). | `docs/` |
| 8.6.3 | TODO | Гайд «Напиши своё приложение для MigaOS». | `docs/` |
| 8.6.4 | TODO | Гайд «Напиши драйвер». | `docs/` |
# Kyronix — roadmap

Target: Linux-compatible x86-64 OS способная запускать реальный userspace без патчей.

`[x]` = done · `[~]` = partial · `[ ]` = not started

Дата составления: июнь 2026. Горизонт: 2 года (до июня 2028).

---

## Реализовано

### Ядро
- [x] GDT / IDT / TSS / PIC 8259 ремап
- [x] PMM (bitmap + free-stack), VMM (4-уровневый PT, NX, HHDM), heap
- [x] Demand paging — #PF выделяет страницу на лету
- [x] PIT ~1000 Hz → g_ticks → вытесняющий планировщик (IRQ0)
- [x] SYSCALL/SYSRET + swapgs, TSS.rsp0, SSE/FPU
- [x] RTC → g_epoch_base (реальное Unix-время)

### Процессы и сигналы
- [x] ELF64 загрузчик + PIE (ET_DYN) + shebang (#!)
- [x] PT_INTERP → динамический линкёр (musl) + AT_BASE в auxv
- [x] fork / execve / wait4 / exit / exit_group
- [x] Кооперативный + вытесняющий планировщик (sched_switch в asm)
- [x] Сигналы: rt_sigaction, rt_sigreturn, kill, SIGCHLD, SIGPIPE
- [x] clone() с CLONE_VM (базовые потоки)
- [x] nanosleep с реальным ожиданием (wakeup_tick + IRQ0 wake)
- [x] futex FUTEX_WAIT / FUTEX_WAKE (реальная блокировка)

### VFS и файловая система
- [x] VFS ramfs: O_CREAT, O_TRUNC, write, unlink, mkdir, rename, chmod, chown
- [x] CPIO initrd, symlinks, chr-dev
- [x] pipe / pipe2 / dup / dup2 / dup3
- [x] /dev/tty, null, zero, urandom, stdin/stdout/stderr
- [x] /proc/version, /proc/self/exe, /proc/self/fd/N
- [x] Per-process cwd; chdir, getcwd, *at-syscalls (mkdirat, unlinkat и др.)

### Память
- [x] mmap (анонимный + file-backed MAP_PRIVATE), mprotect, munmap, mremap, brk
- [x] PROT_EXEC корректно снимает NX-бит
- [x] mmap PROT_EXEC для динамического линкёра

### Syscalls & совместимость
- [x] 150+ syscalls (read/write/open/close, stat, poll, select, epoll, sendfile и др.)
- [x] clock_gettime / gettimeofday (реальное время от RTC + g_ticks)
- [x] getrandom (RDRAND + TSC fallback)
- [x] arch_prctl FS/GS base (TLS)

### Userspace
- [x] musl libc как dynamic linker (/lib/ld-musl-x86_64.so.1)
- [x] /bin/sh → ksh, /usr/bin/env → /bin/env
- [x] ksh: pipes, redirects, history, tilde expansion
- [x] sbase: 50+ POSIX утилит (cat, ls, grep, find, sed, awk, vi, …)
- [x] tcc 0.9.28 (C compiler), NASM (assembler)

---

## Квартал 1 — июнь–август 2026
### Интерактивный shell (то, что видно каждый день)

**Job control в ksh**
- [ ] `&` — запуск в фоне, shell продолжает работу
- [ ] Ctrl+C → SIGINT в foreground process group
- [ ] Ctrl+Z → SIGTSTP, `fg` / `bg` команды
- [ ] TTY: доставлять сигналы через tty на foreground pgid

**Tab completion в ksh**
- [ ] Tab → дополнение имён файлов и команд из PATH
- [ ] Double-Tab → список вариантов

**poll/select реальная блокировка**
- [ ] Сейчас busy-wait — жрёт CPU и ломает event loop программ
- [ ] `fd_wait_queue_t` на каждый fd, процесс → PROC_WAITING до события
- [ ] Разбуждать при read/write-ready (pipe, tty, chr-dev)

**TTY дисциплина**
- [ ] ISIG: Ctrl+C = SIGINT, Ctrl+Z = SIGTSTP, Ctrl+\ = SIGQUIT
- [ ] IXON: Ctrl+S / Ctrl+Q (flow control)
- [ ] canonical mode (строковый) vs raw mode (побайтовый)

---

## Квартал 2 — сентябрь–ноябрь 2026
### Постоянное хранилище

**PCI enumeration**
- [ ] Обход PCI Configuration Space (порты 0xCF8/0xCFC)
- [ ] Найти устройства по vendor/device ID
- [ ] Базовая таблица устройств, BAR mapping

**virtio-blk драйвер**
- [ ] Нашли 1af4:1001 → инициализируем virtqueue
- [ ] Чтение/запись секторов (request + response descriptor ring)
- [ ] Интегрировать с VFS как block device

**ext2 файловая система**
- [ ] Парсинг superblock, group descriptors, inodes
- [ ] Чтение файлов и директорий (read-only для начала)
- [ ] Запись: выделение блоков, создание файлов, директорий
- [ ] fsck-совместимый формат (проверять magic 0xEF53)

**VFS: mount table**
- [ ] `vfs_mount(path, fstype, dev)` / `vfs_umount(path)`
- [ ] Поддержка нескольких точек монтирования одновременно
- [ ] /etc/fstab чтение при boot (tmpfs / ext2)

---

## Квартал 3 — декабрь 2026–февраль 2027
### Сеть: L2/L3

**virtio-net драйвер**
- [ ] PCI device 1af4:1000 → virtqueue RX/TX
- [ ] Отправка и приём ethernet фреймов
- [ ] Interrupt-driven + polling fallback

**Сетевой стек — L2/L3**
- [ ] ARP: reply на запросы, кеш соседей
- [ ] IPv4: fragmentation, checksum, routing table (1 маршрут)
- [ ] ICMP: echo request/reply (ping работает)
- [ ] DHCP клиент (простой: DISCOVER/OFFER/REQUEST/ACK через UDP)

**UDP сокеты**
- [ ] `socket(AF_INET, SOCK_DGRAM)` / `bind` / `sendto` / `recvfrom`
- [ ] Портовый мультиплексор, буферы RX/TX
- [ ] DNS resolver: gethostbyname через UDP :53

---

## Квартал 4 — март–май 2027
### Сеть: TCP + socket API

**TCP стек**
- [ ] State machine: CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT → TIME_WAIT
- [ ] Sliding window, ACK, retransmit timeout (RTO)
- [ ] Passive open: listen/accept

**BSD сокеты**
- [ ] `socket` / `bind` / `listen` / `accept` / `connect`
- [ ] `send` / `recv` / `sendmsg` / `recvmsg`
- [ ] `setsockopt` / `getsockopt` (SO_REUSEADDR, TCP_NODELAY)
- [ ] AF_UNIX (unix domain sockets — нужны для IPC)
- [ ] select/poll/epoll на сокетах

**Утилиты**
- [ ] ping (ICMP), wget/curl (HTTP GET over TCP)
- [ ] nc (netcat), простой HTTP сервер

---

## Квартал 5 — июнь–август 2027
### Потоки (pthreads)

**clone() полная реализация**
- [ ] CLONE_VM + CLONE_FS + CLONE_FILES + CLONE_SIGHAND + CLONE_THREAD
- [ ] Выделение user-stack потоку (если child_stack != 0)
- [ ] Общие fd-таблица, vmm_space, cwd между потоками одного процесса

**TLS (Thread-Local Storage)**
- [ ] Каждый поток имеет свой fs_base (arch_prctl SET_FS)
- [ ] Копирование TLS template из PT_TLS сегмента ELF
- [ ] __thread переменные работают корректно

**Синхронизация**
- [ ] futex FUTEX_REQUEUE / FUTEX_CMP_REQUEUE
- [ ] Правильный exit_thread (не exit_group) для set_tid_address
- [ ] robust futex (FUTEX_WAIT_REQUEUE_PI, грубая реализация)

**Тест**
- [ ] Программа с несколькими pthread_create запускается
- [ ] pthread_mutex_lock/unlock работают через futex

---

## Квартал 6 — сентябрь–ноябрь 2027
### APIC и SMP

**Local APIC**
- [ ] Обнаружение через CPUID + ACPI MADT
- [ ] Включить xAPIC / x2APIC
- [ ] APIC timer как замена PIT (TSC-deadline mode если доступен)
- [ ] Spurious interrupt vector

**I/O APIC**
- [ ] Парсинг MADT, IOAPIC base address
- [ ] Маршрутизация legacy IRQ → векторы
- [ ] Keyboard, serial, timer через IOAPIC

**SMP: AP startup**
- [ ] SIPI sequence (startup IPI → AP переходит в protected/long mode)
- [ ] Per-CPU GDT/IDT/TSS/stack для каждого ядра
- [ ] Spinlock (test-and-set, lock prefix)
- [ ] Per-CPU current process pointer

**SMP: планировщик**
- [ ] Run queue per CPU (or simple global queue с spinlock)
- [ ] IPI для preemption между ядрами
- [ ] Корректный выход из idle через hlt с sti

---

## Квартал 7 — декабрь 2027–февраль 2028
### Безопасность и права доступа

**VFS permissions**
- [ ] Проверка mode bits (rwxrwxrwx) в open/create/exec
- [ ] uid/gid проверяются реально (не захардкожены в 0)
- [ ] sticky bit для /tmp

**Пользователи**
- [ ] /etc/passwd и /etc/shadow парсинг
- [ ] /etc/group
- [ ] login с проверкой пароля (crypt/sha-512)
- [ ] setuid/setgid бинарники (sudo, su)

**chroot и namespaces**
- [ ] `chroot(path)` — меняет корень VFS для процесса
- [ ] Базовая изоляция: CLONE_NEWNS (mount namespace)
- [ ] PID namespace (для контейнеров — stretch goal)

**Capabilities**
- [ ] Базовый набор: CAP_CHOWN, CAP_KILL, CAP_NET_BIND_SERVICE и др.
- [ ] Проверка capabilities вместо «только root»

---

## Квартал 8 — март–июнь 2028
### PTY, продвинутый I/O, полировка

**PTY (псевдотерминал)**
- [ ] /dev/ptmx — открыть мастер
- [ ] /dev/pts/N — slave стороны (devpts псевдо-ФС)
- [ ] Протокол мастер↔slave (TIOCGPTN, TIOCSPTLCK)
- [ ] Нужен для: SSH, tmux, screen, любого terminal multiplexer

**Advanced I/O**
- [ ] inotify (IN_CREATE, IN_MODIFY, IN_DELETE)
- [ ] timerfd / eventfd / signalfd
- [ ] io_uring (submit queue + completion queue) — stretch goal
- [ ] splice / tee (zero-copy между fd)

**Графика (stretch goal)**
- [ ] virtio-gpu Wayland compositor (framebuffer режим)
- [ ] DRM/KMS минимальный интерфейс
- [ ] Простой window manager на framebuffer

**Утилиты и совместимость**
- [ ] ssh (клиент — использует TCP + PTY)
- [ ] Python 3 интерпретатор (статически с musl)
- [ ] Lua / MicroPython как встроенный язык
- [ ] pkg: простейший пакетный менеджер (tar.gz + manifest)
- [ ] /proc/meminfo, /proc/cpuinfo, /proc/net/... полные

---

## Долгосрочные цели (за горизонтом 2028)

- [ ] NVMe драйвер (PCIe, не virtio)
- [ ] USB стек (xHCI → HID keyboard/mouse)
- [ ] Bluetooth (HCI over USB)
- [ ] ACPI: S3 suspend-to-RAM, battery status
- [ ] btrfs или F2FS (copy-on-write ФС)
- [ ] Динамическая линковка полная (dlopen/dlsym/dlclose)
- [ ] VDSO (clock_gettime без syscall через shared page)
- [ ] seccomp-BPF (фильтрация syscalls)
- [ ] cgroups v2 (ограничения ресурсов)
- [ ] Полноценный контейнерный рантайм (как podman)
- [ ] Поддержка RISC-V 64 (второй arch target)

---

## Сводная таблица по годам

| Период | Фокус | Ключевой результат |
|--------|-------|-------------------|
| Q1 2026 | Shell UX | job control, tab completion, real poll |
| Q2 2026 | Хранилище | virtio-blk + ext2 + mount table |
| Q3 2026 | Сеть L2/L3 | ping, DHCP, UDP, DNS |
| Q4 2026 | TCP + сокеты | wget работает, AF_UNIX |
| Q5 2027 | pthreads | многопоточные программы запускаются |
| Q6 2027 | SMP | 2+ ядра, APIC timer |
| Q7 2027 | Безопасность | пользователи, права, chroot |
| Q8 2028 | PTY + polish | tmux, SSH, полная совместимость |

---

## Порядок зависимостей

```
Job control → TTY signals → poll blocking
virtio-blk → ext2 → mount table → persistent /home
virtio-net → ARP/IP → UDP/DNS → TCP → BSD sockets → HTTP
clone threads → TLS → futex improvements → pthreads
APIC → TSC → SMP → per-CPU sched
VFS permissions → users → chroot → namespaces → capabilities
PTY → SSH → tmux
```

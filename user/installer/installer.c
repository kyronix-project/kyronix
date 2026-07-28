#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define INSTALLER_VERSION "0.2.0"
#define MAX_DISKS 16
#define COPY_BUFFER (64u * 1024u)
#define DEFAULT_HOSTNAME "kyronix"
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 0x80081272u
#endif
#define BLKGETSIZE64_COMPAT 0x1262u
#ifndef BLKRRPART
#define BLKRRPART 0x125Fu
#endif
#define BOOT_PARTITION_SECTORS 32768u
#define ROOT_PARTITION_LBA 34816u
#define MINIMUM_DISK_BYTES (128u * 1024u * 1024u)

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} mbr_partition_t;

typedef struct __attribute__((packed)) {
    uint8_t bootstrap[440];
    uint32_t disk_signature;
    uint16_t reserved;
    mbr_partition_t partitions[4];
    uint16_t signature;
} mbr_t;

_Static_assert(sizeof(mbr_t) == 512, "MBR must occupy one sector");

typedef struct {
    char path[64];
    uint64_t bytes;
} disk_info_t;

enum {
    COMPONENT_EDITOR = 1u << 0,
    COMPONENT_NETWORK = 1u << 1,
    COMPONENT_DEVELOPMENT = 1u << 2,
    COMPONENT_SECURITY = 1u << 3,
    COMPONENT_INSTALLER = 1u << 4,
};

typedef struct {
    disk_info_t disk;
    int has_disk;
    uint32_t components;
    int noatime;
    char hostname[64];
} install_config_t;

typedef struct {
    const char *name;
    const char *description;
    uint32_t flag;
} component_info_t;

static const component_info_t component_table[] = {
    {"Text editor", "vi console editor", COMPONENT_EDITOR},
    {"Network tools", "ping, wget, nc and nslookup", COMPONENT_NETWORK},
    {"Development kit", "headers and static libraries",
     COMPONENT_DEVELOPMENT},
    {"Security lab", "Phantom and Anti-TOCTOU self-tests",
     COMPONENT_SECURITY},
    {"Installer tools", "installer, mkfs.ext2 and Limine utility",
     COMPONENT_INSTALLER},
};

static FILE *log_file;
static int ui_active;
static uint64_t copied_files;
static uint64_t copied_bytes;
static char current_step[128];
static const install_config_t *active_config;

static void log_message(const char *message) {
    if (!log_file) return;
    fprintf(log_file, "%s\n", message);
    fflush(log_file);
}

static void ui_shutdown(void) {
    if (!ui_active) return;
    endwin();
    ui_active = 0;
}

static void ui_init(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);
        init_pair(2, COLOR_GREEN, -1);
        init_pair(3, COLOR_RED, -1);
        init_pair(4, COLOR_YELLOW, -1);
    }
    ui_active = 1;
}

static int ui_read_key(void) {
    int key = getch();
    if (key != 27) return key;

    /*
     * Kyronix's tty currently delivers ANSI cursor sequences byte by byte.
     * ncurses therefore sometimes returns ESC before it can translate the
     * remaining "[A"/"[B" bytes into KEY_UP/KEY_DOWN.
     */
    wtimeout(stdscr, 100);
    int prefix = getch();
    if (prefix != '[' && prefix != 'O') {
        wtimeout(stdscr, -1);
        if (prefix != ERR) ungetch(prefix);
        return 27;
    }

    int final = getch();
    for (int i = 0; i < 8 && final != ERR &&
                    ((final >= '0' && final <= '9') || final == ';'); i++)
        final = getch();
    wtimeout(stdscr, -1);

    switch (final) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default: return 27;
    }
}

static void draw_frame(const char *title) {
    erase();
    box(stdscr, 0, 0);
    attron(COLOR_PAIR(1) | A_BOLD);
    mvprintw(1, 3, "Kyronix Installer %s", INSTALLER_VERSION);
    attroff(COLOR_PAIR(1) | A_BOLD);
    mvhline(2, 1, ACS_HLINE, COLS - 2);
    attron(A_BOLD);
    mvprintw(4, 4, "%s", title);
    attroff(A_BOLD);
}

static void show_error(const char *message) {
    if (!ui_active) {
        fprintf(stderr, "installer: %s\n", message);
        return;
    }
    draw_frame("Installation error");
    attron(COLOR_PAIR(3) | A_BOLD);
    mvprintw(7, 4, "%s", message);
    attroff(COLOR_PAIR(3) | A_BOLD);
    mvprintw(9, 4, "Details: /tmp/kyronix-install.log");
    mvprintw(LINES - 3, 4, "Press any key to return.");
    refresh();
    getch();
}

static uint64_t get_device_size(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) < 0 &&
        ioctl(fd, BLKGETSIZE64_COMPAT, &bytes) < 0) {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end > 0) bytes = (uint64_t) end;
    }
    close(fd);
    return bytes;
}

static int is_disk_name(const char *name) {
    return strlen(name) == 3 && name[0] == 's' && name[1] == 'd' &&
           name[2] >= 'a' && name[2] <= 'z';
}

static int discover_disks(disk_info_t disks[MAX_DISKS]) {
    DIR *dir = opendir("/dev");
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < MAX_DISKS) {
        if (!is_disk_name(entry->d_name)) continue;
        snprintf(disks[count].path, sizeof(disks[count].path), "/dev/%s",
                 entry->d_name);
        disks[count].bytes = get_device_size(disks[count].path);
        if (disks[count].bytes >= MINIMUM_DISK_BYTES) count++;
    }
    closedir(dir);

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(disks[i].path, disks[j].path) <= 0) continue;
            disk_info_t tmp = disks[i];
            disks[i] = disks[j];
            disks[j] = tmp;
        }
    }
    return count;
}

static void format_size(uint64_t bytes, char *out, size_t out_size) {
    const uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    if (bytes >= gib)
        snprintf(out, out_size, "%llu.%llu GiB",
                 (unsigned long long) (bytes / gib),
                 (unsigned long long) ((bytes % gib) * 10ULL / gib));
    else
        snprintf(out, out_size, "%llu MiB",
                 (unsigned long long) (bytes / (1024ULL * 1024ULL)));
}

static int select_disk(disk_info_t disks[MAX_DISKS], int count) {
    int selected = 0;
    while (1) {
        draw_frame("Select installation disk");
        mvprintw(6, 4, "The selected disk will be completely erased.");
        for (int i = 0; i < count; i++) {
            char size[32];
            format_size(disks[i].bytes, size, sizeof(size));
            if (i == selected) attron(A_REVERSE);
            mvprintw(8 + i, 6, "%-16s %12s", disks[i].path, size);
            if (i == selected) attroff(A_REVERSE);
        }
        mvprintw(LINES - 3, 4, "Up/Down: select   Enter: continue   Q: quit");
        refresh();

        int key = ui_read_key();
        if ((key == KEY_UP || key == 'k') && selected > 0) selected--;
        else if ((key == KEY_DOWN || key == 'j') && selected + 1 < count)
            selected++;
        else if (key == '\n' || key == KEY_ENTER) return selected;
        else if (key == 'q' || key == 'Q' || key == 27) return -1;
    }
}

static int valid_hostname(const char *hostname) {
    size_t length = strlen(hostname);
    if (length == 0 || length > 63 || hostname[0] == '-' ||
        hostname[length - 1] == '-')
        return 0;
    for (size_t i = 0; i < length; i++) {
        char c = hostname[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-')
            continue;
        return 0;
    }
    return 1;
}

static void show_information(const char *title, const char *line1,
                             const char *line2) {
    draw_frame(title);
    mvprintw(7, 4, "%s", line1);
    if (line2 && line2[0]) mvprintw(9, 4, "%s", line2);
    mvprintw(LINES - 3, 4, "Press any key to return.");
    refresh();
    getch();
}

static void edit_hostname(install_config_t *config) {
    char value[sizeof(config->hostname)];
    while (1) {
        snprintf(value, sizeof(value), "%s", config->hostname);
        draw_frame("System hostname");
        mvprintw(7, 4, "Hostname:");
        move(9, 4);
        clrtoeol();
        echo();
        curs_set(1);
        getnstr(value, (int) sizeof(value) - 1);
        noecho();
        curs_set(0);
        if (valid_hostname(value)) {
            snprintf(config->hostname, sizeof(config->hostname), "%s", value);
            return;
        }
        show_information("Invalid hostname",
                         "Use 1-63 letters, digits or hyphens.",
                         "The first and last character cannot be a hyphen.");
    }
}

static void edit_components(install_config_t *config) {
    int selected = 0;
    int count = (int) (sizeof(component_table) / sizeof(component_table[0]));
    while (1) {
        draw_frame("Base system components");
        mvprintw(6, 4, "These components are copied from the live image.");
        for (int i = 0; i < count; i++) {
            int enabled = (config->components & component_table[i].flag) != 0;
            if (i == selected) attron(A_REVERSE);
            mvprintw(8 + i * 2, 6, "[%c] %-20s %s",
                     enabled ? 'x' : ' ', component_table[i].name,
                     component_table[i].description);
            if (i == selected) attroff(A_REVERSE);
        }
        mvprintw(LINES - 3, 4,
                 "Up/Down: select   Space: toggle   Enter/Esc: return");
        refresh();

        int key = ui_read_key();
        if ((key == KEY_UP || key == 'k') && selected > 0) selected--;
        else if ((key == KEY_DOWN || key == 'j') && selected + 1 < count)
            selected++;
        else if (key == ' ') {
            config->components ^= component_table[selected].flag;
        } else if (key == '\n' || key == KEY_ENTER || key == 27 ||
                   key == 'q' || key == 'Q') {
            return;
        }
    }
}

static void edit_disk(install_config_t *config) {
    disk_info_t disks[MAX_DISKS];
    int count = discover_disks(disks);
    if (count == 0) {
        show_information("No suitable disks",
                         "No unfiltered /dev/sdX disk of at least 128 MiB found.",
                         "Attach a disk and open this row again.");
        return;
    }
    int index = select_disk(disks, count);
    if (index >= 0) {
        config->disk = disks[index];
        config->has_disk = 1;
    }
}

static void draw_config_row(int row, int selected, const char *label,
                            const char *value) {
    if (selected) attron(A_REVERSE);
    mvprintw(row, 5, " %-18s  %-45s ", label, value);
    if (selected) attroff(A_REVERSE);
}

static int configure_install(install_config_t *config) {
    static const int row_count = 9;
    int selected = 0;

    while (1) {
        char disk_value[96];
        char component_value[64];
        char mount_value[32];
        if (config->has_disk) {
            char size[32];
            format_size(config->disk.bytes, size, sizeof(size));
            snprintf(disk_value, sizeof(disk_value), "%s (%s)",
                     config->disk.path, size);
        } else {
            snprintf(disk_value, sizeof(disk_value), "Not selected");
        }
        int enabled = 0;
        for (int i = 0; i < (int) (sizeof(component_table) /
                                    sizeof(component_table[0])); i++)
            if (config->components & component_table[i].flag) enabled++;
        snprintf(component_value, sizeof(component_value), "%d of %zu enabled",
                 enabled, sizeof(component_table) / sizeof(component_table[0]));
        snprintf(mount_value, sizeof(mount_value), "%s",
                 config->noatime ? "noatime" : "relatime");

        draw_frame("Installation configuration");
        mvprintw(5, 5, "Configure each row, then select Install.");
        draw_config_row(7, selected == 0, "Target disk", disk_value);
        draw_config_row(8, selected == 1, "Partitioning",
                        "Automatic MBR (erase disk)");
        draw_config_row(9, selected == 2, "Root filesystem", "ext2");
        draw_config_row(10, selected == 3, "Mount policy", mount_value);
        draw_config_row(11, selected == 4, "Packages", component_value);
        draw_config_row(12, selected == 5, "Hostname", config->hostname);
        draw_config_row(13, selected == 6, "Bootloader", "Limine (Legacy BIOS)");
        draw_config_row(15, selected == 7, "Install", "Review and start");
        draw_config_row(16, selected == 8, "Exit", "Leave installer");
        if (!config->has_disk) {
            attron(COLOR_PAIR(4));
            mvprintw(19, 5, "Select a target disk before installation.");
            attroff(COLOR_PAIR(4));
        }
        mvprintw(LINES - 3, 4,
                 "Up/Down: navigate   Enter: edit/select   Q: exit");
        refresh();

        int key = ui_read_key();
        if ((key == KEY_UP || key == 'k') && selected > 0) selected--;
        else if ((key == KEY_DOWN || key == 'j') &&
                 selected + 1 < row_count)
            selected++;
        else if (key == 'q' || key == 'Q' || key == 27) return 0;
        else if (key != '\n' && key != KEY_ENTER &&
                 key != KEY_LEFT && key != KEY_RIGHT)
            continue;
        else if (selected == 0)
            edit_disk(config);
        else if (selected == 1)
            show_information("Automatic partitioning",
                             "Creates a 16 MiB FAT16 boot partition and",
                             "uses the remaining space for the ext2 root.");
        else if (selected == 2)
            show_information("Root filesystem",
                             "ext2 is the writable root filesystem supported",
                             "by the current Kyronix kernel.");
        else if (selected == 3)
            config->noatime = !config->noatime;
        else if (selected == 4)
            edit_components(config);
        else if (selected == 5)
            edit_hostname(config);
        else if (selected == 6)
            show_information("Bootloader",
                             "The current installer supports Limine in",
                             "Legacy BIOS mode. UEFI will be added later.");
        else if (selected == 7) {
            if (!config->has_disk) {
                show_information("Target disk required",
                                 "Select a target disk in the first row.", "");
            } else {
                return 1;
            }
        } else if (selected == 8) {
            return 0;
        }
    }
}

static int confirm_erase(const disk_info_t *disk) {
    char answer[32];
    char size[32];
    format_size(disk->bytes, size, sizeof(size));

    draw_frame("Confirm destructive installation");
    attron(COLOR_PAIR(3) | A_BOLD);
    mvprintw(6, 4, "ALL DATA ON %s (%s) WILL BE LOST.", disk->path, size);
    attroff(COLOR_PAIR(3) | A_BOLD);
    mvprintw(8, 4, "Layout: MBR / FAT16 boot / ext2 root / Legacy BIOS");
    mvprintw(10, 4, "Type INSTALL to continue:");
    move(12, 4);
    echo();
    curs_set(1);
    getnstr(answer, (int) sizeof(answer) - 1);
    noecho();
    curs_set(0);
    return strcmp(answer, "INSTALL") == 0;
}

static void show_progress(const char *step, int index, int total) {
    if (step != current_step)
        snprintf(current_step, sizeof(current_step), "%s", step);
    if (!ui_active) {
        printf("[%d/%d] %s\n", index, total, step);
        fflush(stdout);
        return;
    }

    draw_frame("Installing Kyronix");
    mvprintw(7, 4, "Target: %s", current_step);
    int width = COLS - 10;
    int filled = total ? width * index / total : 0;
    mvprintw(10, 4, "[");
    for (int i = 0; i < width; i++) addch(i < filled ? ACS_CKBOARD : ' ');
    addch(']');
    mvprintw(12, 4, "Files: %llu   Copied: %llu MiB",
             (unsigned long long) copied_files,
             (unsigned long long) (copied_bytes / (1024ULL * 1024ULL)));
    mvprintw(LINES - 3, 4, "Do not power off the computer.");
    refresh();
}

static int valid_install_disk(const char *path) {
    return path && strncmp(path, "/dev/", 5) == 0 &&
           is_disk_name(path + 5);
}

static int disk_is_mounted(const char *disk) {
    FILE *mounts = fopen("/proc/mounts", "r");
    if (!mounts) mounts = fopen("/etc/mtab", "r");
    if (!mounts) return 0;

    char source[256];
    char mountpoint[256];
    char filesystem[64];
    char options[256];
    int mounted = 0;
    while (fscanf(mounts, "%255s %255s %63s %255s %*d %*d",
                  source, mountpoint, filesystem, options) == 4) {
        size_t length = strlen(disk);
        if (strncmp(source, disk, length) == 0 &&
            (source[length] == '\0' ||
             (source[length] >= '0' && source[length] <= '9'))) {
            mounted = 1;
            break;
        }
    }
    fclose(mounts);
    return mounted;
}

static int run_command(char *const argv[]) {
    if (log_file) {
        fprintf(log_file, "$");
        for (int i = 0; argv[i]; i++) fprintf(log_file, " %s", argv[i]);
        fputc('\n', log_file);
        fflush(log_file);
    }

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (log_file) {
            int fd = fileno(log_file);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int write_all(int fd, const void *buffer, size_t size) {
    const uint8_t *bytes = buffer;
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, bytes + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        done += (size_t) n;
    }
    return 0;
}

static int partition_disk(const char *disk, char *boot_partition,
                          size_t boot_partition_size, char *root_partition,
                          size_t root_partition_size) {
    uint64_t bytes = get_device_size(disk);
    uint64_t sectors64 = bytes / 512u;
    if (sectors64 <= ROOT_PARTITION_LBA + 4096u ||
        sectors64 - ROOT_PARTITION_LBA > UINT32_MAX) {
        errno = EFBIG;
        return -1;
    }

    int fd = open(disk, O_RDWR);
    if (fd < 0) return -1;

    mbr_t mbr;
    memset(&mbr, 0, sizeof(mbr));
    mbr.disk_signature =
        (uint32_t) getpid() ^ (uint32_t) bytes ^ (uint32_t) (bytes >> 32);
    mbr.partitions[0].status = 0x80;
    mbr.partitions[0].chs_first[0] = 0x00;
    mbr.partitions[0].chs_first[1] = 0x02;
    mbr.partitions[0].chs_first[2] = 0x00;
    mbr.partitions[0].type = 0x0E;
    memset(mbr.partitions[0].chs_last, 0xFF, 3);
    mbr.partitions[0].lba_first = 2048;
    mbr.partitions[0].sectors = BOOT_PARTITION_SECTORS;
    mbr.partitions[1].type = 0x83;
    memset(mbr.partitions[1].chs_first, 0xFF, 3);
    memset(mbr.partitions[1].chs_last, 0xFF, 3);
    mbr.partitions[1].lba_first = ROOT_PARTITION_LBA;
    mbr.partitions[1].sectors =
        (uint32_t) (sectors64 - ROOT_PARTITION_LBA);
    mbr.signature = 0xAA55;

    if (lseek(fd, 0, SEEK_SET) < 0 || write_all(fd, &mbr, sizeof(mbr)) < 0 ||
        fsync(fd) < 0) {
        close(fd);
        return -1;
    }

    if (ioctl(fd, BLKRRPART, 0) < 0) {
        close(fd);
        return -1;
    }
    close(fd);

    snprintf(boot_partition, boot_partition_size, "%s1", disk);
    snprintf(root_partition, root_partition_size, "%s2", disk);
    for (int attempt = 0; attempt < 50; attempt++) {
        if (access(boot_partition, F_OK) == 0 &&
            access(root_partition, F_OK) == 0)
            return 0;
        usleep(100000);
    }
    errno = ENOENT;
    return -1;
}

static int copy_boot_image(const char *partition) {
    int out = open(partition, O_WRONLY);
    if (out < 0) return -1;

    uint8_t *buffer = malloc(COPY_BUFFER);
    if (!buffer) {
        close(out);
        return -1;
    }

    int result = 0;
    uint64_t total = 0;
    for (int chunk = 0; chunk < 16 && result == 0; chunk++) {
        char path[64];
        snprintf(path, sizeof(path),
                 "/usr/share/kyronix/boot.fat.%02d", chunk);
        int in = open(path, O_RDONLY);
        if (in < 0) {
            result = -1;
            break;
        }
        while (1) {
            ssize_t size = read(in, buffer, COPY_BUFFER);
            if (size == 0) break;
            if (size < 0) {
                if (errno == EINTR) continue;
                result = -1;
                break;
            }
            if (write_all(out, buffer, (size_t) size) < 0) {
                result = -1;
                break;
            }
            total += (uint64_t) size;
        }
        close(in);
    }
    if (result == 0 &&
        total != (uint64_t) BOOT_PARTITION_SECTORS * 512u) {
        errno = EIO;
        result = -1;
    }
    if (result == 0 && fsync(out) < 0) result = -1;
    free(buffer);
    close(out);
    return result;
}

static int ensure_directory(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0) return 0;
    if (errno != EEXIST) return -1;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int copy_entry(const char *source, const char *target);

static int path_is(const char *path, const char *candidate) {
    return strcmp(path, candidate) == 0;
}

static int path_is_below(const char *path, const char *directory) {
    size_t length = strlen(directory);
    return strncmp(path, directory, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

static int excluded_component_path(const char *source) {
    if (!active_config) return 0;
    uint32_t components = active_config->components;

    if (!(components & COMPONENT_EDITOR) && path_is(source, "/bin/vi"))
        return 1;
    if (!(components & COMPONENT_NETWORK) &&
        (path_is(source, "/bin/nc") ||
         path_is(source, "/bin/nslookup") ||
         path_is(source, "/bin/ping") ||
         path_is(source, "/bin/wget")))
        return 1;
    if (!(components & COMPONENT_DEVELOPMENT) &&
        (path_is_below(source, "/usr/include") ||
         path_is(source, "/usr/lib/libc.a")))
        return 1;
    if (!(components & COMPONENT_INSTALLER) &&
        (path_is(source, "/bin/installer") ||
         path_is(source, "/bin/mkfs.ext2") ||
         path_is(source, "/usr/libexec/limine-install")))
        return 1;
    return 0;
}

static int copy_directory(const char *source, const char *target, const struct stat *st) {
    if (ensure_directory(target, st->st_mode & 07777u) < 0) return -1;
    DIR *dir = opendir(source);
    if (!dir) return -1;

    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char child_source[1024];
        char child_target[1024];
        if (snprintf(child_source, sizeof(child_source), "%s/%s", source,
                     entry->d_name) >= (int) sizeof(child_source) ||
            snprintf(child_target, sizeof(child_target), "%s/%s", target,
                     entry->d_name) >= (int) sizeof(child_target)) {
            errno = ENAMETOOLONG;
            result = -1;
            break;
        }
        if (copy_entry(child_source, child_target) < 0) {
            result = -1;
            break;
        }
    }
    closedir(dir);
    chmod(target, st->st_mode & 07777u);
    return result;
}

static int copy_regular(const char *source, const char *target, const struct stat *st) {
    int in = open(source, O_RDONLY);
    if (in < 0) return -1;
    int out = open(target, O_WRONLY | O_CREAT | O_TRUNC, st->st_mode & 07777u);
    if (out < 0) {
        close(in);
        return -1;
    }

    uint8_t *buffer = malloc(COPY_BUFFER);
    if (!buffer) {
        close(in);
        close(out);
        return -1;
    }

    int result = 0;
    while (1) {
        ssize_t n = read(in, buffer, COPY_BUFFER);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            result = -1;
            break;
        }
        if (write_all(out, buffer, (size_t) n) < 0) {
            result = -1;
            break;
        }
        copied_bytes += (uint64_t) n;
    }
    free(buffer);
    if (fsync(out) < 0) result = -1;
    close(in);
    close(out);
    chmod(target, st->st_mode & 07777u);
    copied_files++;
    if (ui_active && copied_files % 10u == 0)
        show_progress(current_step, 6, 9);
    return result;
}

static int copy_entry(const char *source, const char *target) {
    static const char boot_chunk_prefix[] =
        "/usr/share/kyronix/boot.fat.";
    if (strncmp(source, boot_chunk_prefix,
                sizeof(boot_chunk_prefix) - 1u) == 0)
        return 0;
    if (excluded_component_path(source)) return 0;

    struct stat st;
    if (lstat(source, &st) < 0) return -1;

    if (S_ISDIR(st.st_mode)) return copy_directory(source, target, &st);
    if (S_ISREG(st.st_mode)) return copy_regular(source, target, &st);
    if (S_ISLNK(st.st_mode)) {
        char link_target[1024];
        ssize_t size = readlink(source, link_target, sizeof(link_target) - 1);
        if (size < 0) return -1;
        link_target[size] = '\0';
        unlink(target);
        if (symlink(link_target, target) < 0) return -1;
        copied_files++;
    }
    return 0;
}

static int excluded_root_entry(const char *name) {
    static const char *excluded[] = {
        "dev", "proc", "sys", "mnt", "tmp", "run", NULL,
    };
    for (int i = 0; excluded[i]; i++)
        if (strcmp(name, excluded[i]) == 0) return 1;
    return 0;
}

static int copy_rootfs(const char *target) {
    DIR *root = opendir("/");
    if (!root) return -1;
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(root)) != NULL) {
        if (entry->d_name[0] == '\0' || strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            excluded_root_entry(entry->d_name))
            continue;

        char source[512];
        char destination[1024];
        snprintf(source, sizeof(source), "/%s", entry->d_name);
        snprintf(destination, sizeof(destination), "%s/%s", target,
                 entry->d_name);
        if (copy_entry(source, destination) < 0) {
            result = -1;
            break;
        }
    }
    closedir(root);
    if (result < 0) return -1;

    char path[1024];
    static const char *directories[] = {
        "dev", "proc", "sys", "mnt", "tmp", "run", NULL,
    };
    for (int i = 0; directories[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", target, directories[i]);
        mode_t mode = strcmp(directories[i], "tmp") == 0 ? 01777 : 0755;
        if (ensure_directory(path, mode) < 0) return -1;
        chmod(path, mode);
    }
    return 0;
}

static int write_fstab(const char *target, const char *partition,
                       int noatime) {
    char etc[1024];
    char path[1024];
    snprintf(etc, sizeof(etc), "%s/etc", target);
    if (ensure_directory(etc, 0755) < 0) return -1;
    snprintf(path, sizeof(path), "%s/etc/fstab", target);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    char line[256];
    int length = snprintf(line, sizeof(line),
                          "%s / ext2 rw,%s 0 1\n", partition,
                          noatime ? "noatime" : "relatime");
    int result = write_all(fd, line, (size_t) length);
    if (fsync(fd) < 0) result = -1;
    close(fd);
    return result;
}

static int write_hostname(const char *target, const char *hostname) {
    char etc[1024];
    char path[1024];
    snprintf(etc, sizeof(etc), "%s/etc", target);
    if (ensure_directory(etc, 0755) < 0) return -1;
    snprintf(path, sizeof(path), "%s/etc/hostname", target);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    char line[80];
    int length = snprintf(line, sizeof(line), "%s\n", hostname);
    int result = write_all(fd, line, (size_t) length);
    if (fsync(fd) < 0) result = -1;
    close(fd);
    return result;
}

static int preflight(void) {
    static const char *required[] = {
        "/bin/mkfs.ext2",
        "/usr/libexec/limine-install",
        "/usr/share/kyronix/boot.fat.00",
        "/usr/share/kyronix/boot.fat.15",
        "/boot/kernel.elf",
        "/boot/limine/limine.conf",
        "/boot/limine/limine-bios.sys",
        NULL,
    };
    for (int i = 0; required[i]; i++) {
        if (access(required[i], R_OK) == 0) continue;
        if (log_file)
            fprintf(log_file, "missing installer payload: %s\n", required[i]);
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static int perform_install(const install_config_t *config) {
    const disk_info_t *disk = &config->disk;
    const char *mountpoint = "/mnt/kyronix-target";
    char boot_partition[80];
    char root_partition[80];
    char error[256];
    int mounted = 0;

    copied_files = 0;
    copied_bytes = 0;
    active_config = config;

    show_progress("Checking installation payload", 1, 9);
    if (preflight() < 0) goto fail;

    show_progress("Writing MBR partition table", 2, 9);
    if (partition_disk(disk->path, boot_partition, sizeof(boot_partition),
                       root_partition, sizeof(root_partition)) < 0)
        goto fail;

    show_progress("Writing FAT16 boot filesystem", 3, 9);
    if (copy_boot_image(boot_partition) < 0) goto fail;

    show_progress("Creating ext2 root filesystem", 4, 9);
    char *mkfs_argv[] = {
        "/bin/mkfs.ext2", "-F", "-L", "kyronix", root_partition, NULL,
    };
    if (run_command(mkfs_argv) != 0) {
        errno = EIO;
        goto fail;
    }

    if (ensure_directory("/mnt", 0755) < 0 ||
        ensure_directory(mountpoint, 0755) < 0)
        goto fail;
    show_progress("Mounting root filesystem", 5, 9);
    if (mount(root_partition, mountpoint, "ext2", 0, NULL) < 0) goto fail;
    mounted = 1;

    show_progress("Copying Kyronix system", 6, 9);
    if (copy_rootfs(mountpoint) < 0) goto fail;
    if (write_fstab(mountpoint, root_partition, config->noatime) < 0)
        goto fail;
    if (write_hostname(mountpoint, config->hostname) < 0) goto fail;
    sync();

    show_progress("Finalizing filesystem", 7, 9);
    if (umount2(mountpoint, 0) < 0) goto fail;
    mounted = 0;

    show_progress("Installing Limine bootloader", 8, 9);
    char *limine_argv[] = {
        "/usr/libexec/limine-install", "bios-install", (char *) disk->path, NULL,
    };
    if (run_command(limine_argv) != 0) {
        errno = EIO;
        goto fail;
    }
    sync();

    show_progress("Installation complete", 9, 9);
    log_message("installation completed successfully");
    active_config = NULL;
    return 0;

fail:
    snprintf(error, sizeof(error), "%s: %s", current_step, strerror(errno));
    if (log_file) fprintf(log_file, "ERROR: %s\n", error);
    if (mounted) {
        sync();
        umount2(mountpoint, 0);
    }
    active_config = NULL;
    show_error(error);
    return -1;
}

static void show_success(const disk_info_t *disk) {
    if (!ui_active) {
        printf("Kyronix installed successfully on %s\n", disk->path);
        return;
    }
    draw_frame("Installation complete");
    attron(COLOR_PAIR(2) | A_BOLD);
    mvprintw(7, 4, "Kyronix was installed successfully on %s.", disk->path);
    attroff(COLOR_PAIR(2) | A_BOLD);
    mvprintw(9, 4, "Remove the installation media and reboot.");
    mvprintw(LINES - 3, 4, "Press any key to exit.");
    refresh();
    getch();
}

static void print_plan(const install_config_t *config) {
    const disk_info_t *disk = &config->disk;
    char size[32];
    format_size(disk->bytes, size, sizeof(size));
    printf("Kyronix installer dry-run\n");
    printf("  target:     %s (%s)\n", disk->path, size);
    printf("  boot:       %s1, FAT16, 16 MiB, active, Limine BIOS\n",
           disk->path);
    printf("  root:       %s2, ext2, label kyronix\n", disk->path);
    printf("  mount:      rw,%s\n",
           config->noatime ? "noatime" : "relatime");
    printf("  hostname:   %s\n", config->hostname);
    printf("  components:");
    for (int i = 0; i < (int) (sizeof(component_table) /
                                sizeof(component_table[0])); i++)
        if (config->components & component_table[i].flag)
            printf(" %s", component_table[i].name);
    printf("\n");
    printf("  source:     current live rootfs\n");
    printf("No changes were made.\n");
}

static void usage(const char *program) {
    printf("usage: %s [--target /dev/sdX] [--hostname NAME] "
           "[--yes] [--dry-run]\n", program);
    printf("  --target PATH  preselect a disk\n");
    printf("  --hostname NAME set the installed system hostname\n");
    printf("  --yes          skip ncurses confirmation (requires --target)\n");
    printf("  --dry-run      print the installation plan without writing\n");
}

int main(int argc, char **argv) {
    const char *target = NULL;
    int assume_yes = 0;
    int dry_run = 0;
    install_config_t config;
    memset(&config, 0, sizeof(config));
    config.components =
        COMPONENT_EDITOR | COMPONENT_NETWORK | COMPONENT_SECURITY;
    config.noatime = 1;
    snprintf(config.hostname, sizeof(config.hostname), "%s",
             DEFAULT_HOSTNAME);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc)
            target = argv[++i];
        else if (strcmp(argv[i], "--hostname") == 0 && i + 1 < argc) {
            const char *hostname = argv[++i];
            if (!valid_hostname(hostname)) {
                fprintf(stderr, "installer: invalid hostname '%s'\n",
                        hostname);
                return 2;
            }
            snprintf(config.hostname, sizeof(config.hostname), "%s",
                     hostname);
        }
        else if (strcmp(argv[i], "--yes") == 0)
            assume_yes = 1;
        else if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = 1;
        else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (assume_yes && !target) {
        fprintf(stderr, "installer: --yes requires --target\n");
        return 2;
    }
    if (getuid() != 0 && !dry_run) {
        fprintf(stderr, "installer: root privileges are required\n");
        return 1;
    }

    log_file = fopen("/tmp/kyronix-install.log", "w");
    if (log_file)
        fprintf(log_file, "Kyronix Installer %s\n", INSTALLER_VERSION);

    if (target) {
        snprintf(config.disk.path, sizeof(config.disk.path), "%s", target);
        config.disk.bytes = get_device_size(target);
        if (config.disk.bytes < MINIMUM_DISK_BYTES) {
            fprintf(stderr,
                    "installer: target is missing or smaller than 128 MiB\n");
            if (log_file) fclose(log_file);
            return 1;
        }
        config.has_disk = 1;
    } else {
        ui_init();
        if (!configure_install(&config)) {
            ui_shutdown();
            if (log_file) fclose(log_file);
            return 0;
        }
    }

    if (dry_run) {
        if (ui_active) ui_shutdown();
        print_plan(&config);
        if (log_file) fclose(log_file);
        return 0;
    }

    if (!valid_install_disk(config.disk.path)) {
        if (ui_active) ui_shutdown();
        fprintf(stderr, "installer: real installation requires /dev/sdX\n");
        if (log_file) fclose(log_file);
        return 1;
    }
    if (disk_is_mounted(config.disk.path)) {
        if (ui_active) ui_shutdown();
        fprintf(stderr, "installer: %s or one of its partitions is mounted\n",
                config.disk.path);
        if (log_file) fclose(log_file);
        return 1;
    }

    if (!assume_yes) {
        if (!ui_active) ui_init();
        if (!confirm_erase(&config.disk)) {
            ui_shutdown();
            printf("Installation cancelled.\n");
            if (log_file) fclose(log_file);
            return 0;
        }
    }

    int result = perform_install(&config);
    if (result == 0) show_success(&config.disk);
    ui_shutdown();
    if (log_file) fclose(log_file);
    return result == 0 ? 0 : 1;
}

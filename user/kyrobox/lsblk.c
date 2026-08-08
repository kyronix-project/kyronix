#include "common.h"
#include <sys/ioctl.h>

#define BLKGETSIZE64 0x80081272UL
#define BLKGETSIZE64_COMPAT 0x1262UL

typedef struct {
    char name[NAME_MAX + 1];
    uint64_t size;
    int partition;
    char mountpoint[PATH_MAX];
} block_entry_t;

static int is_block_name(const char *name) {
    if (strncmp(name, "sd", 2) == 0 || strncmp(name, "vd", 2) == 0 ||
        strncmp(name, "hd", 2) == 0)
        return isalpha((unsigned char)name[2]) && name[3] != '\0';
    if (strncmp(name, "nvme", 4) == 0)
        return isdigit((unsigned char)name[4]);
    return 0;
}

static int is_partition_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0) return 0;
    if (strncmp(name, "nvme", 4) == 0) {
        const char *p = strstr(name, "p");
        return p && p[1] && isdigit((unsigned char)p[1]);
    }
    return isdigit((unsigned char)name[len - 1]);
}

static int compare_entries(const void *a, const void *b) {
    const block_entry_t *left = a;
    const block_entry_t *right = b;
    return strcmp(left->name, right->name);
}

static uint64_t block_size(int fd) {
    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) == 0 ||
        ioctl(fd, BLKGETSIZE64_COMPAT, &size) == 0)
        return size;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) return (uint64_t)st.st_size;
    return 0;
}

static void format_size(uint64_t bytes, char *out, size_t size, int raw) {
    if (raw) {
        snprintf(out, size, "%llu", (unsigned long long)bytes);
        return;
    }
    const char *units[] = {"B", "K", "M", "G", "T"};
    int unit = 0;
    double value = (double)bytes;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) snprintf(out, size, "%lluB", (unsigned long long)bytes);
    else snprintf(out, size, "%.1f%s", value, units[unit]);
}

static void find_mountpoint(const char *name, char *out, size_t size) {
    out[0] = '\0';
    FILE *mounts = fopen("/proc/mounts", "r");
    if (!mounts) return;
    char device[PATH_MAX], mount[PATH_MAX], type[64], options[256];
    while (fscanf(mounts, "%255s %255s %63s %255s %*d %*d\n",
                  device, mount, type, options) == 4) {
        char expected[PATH_MAX];
        snprintf(expected, sizeof(expected), "/dev/%s", name);
        if (strcmp(device, expected) == 0) {
            snprintf(out, size, "%s", mount);
            break;
        }
    }
    fclose(mounts);
}

int main(int argc, char **argv) {
    kx_prog = "lsblk";
    int raw_size = 0;
    int first = 1;
    while (first < argc && argv[first][0] == '-') {
        if (strcmp(argv[first], "-b") == 0 || strcmp(argv[first], "--bytes") == 0)
            raw_size = 1;
        else if (strcmp(argv[first], "-h") == 0 || strcmp(argv[first], "--help") == 0) {
            puts("usage: lsblk [-b|--bytes]");
            return 0;
        } else {
            fprintf(stderr, "lsblk: unknown option: %s\n", argv[first]);
            return 2;
        }
        first++;
    }
    if (first != argc) {
        fprintf(stderr, "lsblk: device arguments are not supported\n");
        return 2;
    }

    DIR *dir = opendir("/dev");
    if (!dir) {
        kx_warn("/dev");
        return 1;
    }
    block_entry_t entries[128];
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < sizeof(entries) / sizeof(entries[0])) {
        if (!is_block_name(entry->d_name)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        struct stat st;
        if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode)) {
            close(fd);
            continue;
        }
        block_entry_t *item = &entries[count++];
        snprintf(item->name, sizeof(item->name), "%s", entry->d_name);
        item->size = block_size(fd);
        item->partition = is_partition_name(item->name);
        find_mountpoint(item->name, item->mountpoint, sizeof(item->mountpoint));
        close(fd);
    }
    closedir(dir);
    qsort(entries, count, sizeof(entries[0]), compare_entries);

    puts("NAME       SIZE       TYPE MOUNTPOINT");
    for (size_t i = 0; i < count; i++) {
        char size[32];
        format_size(entries[i].size, size, sizeof(size), raw_size);
        printf("%-10s %-10s %-4s %s\n", entries[i].name, size,
               entries[i].partition ? "part" : "disk", entries[i].mountpoint);
    }
    return 0;
}

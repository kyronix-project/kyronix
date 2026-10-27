#include "common.h"
#include <sys/ioctl.h>

#define BLOCK_SIZE 512
#define PART_ENTRIES 4
#define PART_SIZE 16
#define MBR_SIZE 512
#define MBR_SIG_OFFSET 510
#define MBR_SIG 0x55AA

#define IOCTL_BLKGETSIZE64 0x1262
#define IOCTL_BLKSSZGET 0x1276

typedef struct {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} __attribute__((packed)) part_entry_t;

typedef struct {
    uint8_t bootstrap[440];
    uint32_t disk_sig;
    uint16_t reserved;
    part_entry_t parts[PART_ENTRIES];
    uint16_t sig;
} __attribute__((packed)) mbr_t;

static const char *part_type_name(uint8_t t) {
    switch (t) {
    case 0x00:
        return "Empty";
    case 0x01:
        return "FAT12";
    case 0x04:
        return "FAT16 <32M";
    case 0x06:
        return "FAT16 >32M";
    case 0x07:
        return "NTFS/exFAT";
    case 0x0B:
        return "W95 FAT32";
    case 0x0C:
        return "W95 FAT32 (LBA)";
    case 0x82:
        return "Linux swap";
    case 0x83:
        return "Linux";
    case 0x85:
        return "Linux extended";
    case 0xEE:
        return "GPT protective";
    default:
        return "Unknown";
    }
}

static int parse_type(const char *s) {
    if (strcmp(s, "linux") == 0 || strcmp(s, "ext2") == 0) return 0x83;
    if (strcmp(s, "swap") == 0) return 0x82;
    if (strcmp(s, "fat12") == 0) return 0x01;
    if (strcmp(s, "fat16") == 0) return 0x06;
    if (strcmp(s, "fat32") == 0) return 0x0B;
    if (strcmp(s, "ntfs") == 0) return 0x07;
    char *end;
    long v = strtol(s, &end, 0);
    if (*end == 0 && v >= 0 && v <= 255) return v;
    return -1;
}

static uint64_t get_disk_size(int fd) {
    uint64_t size = 0;
    if (ioctl(fd, IOCTL_BLKGETSIZE64, &size) < 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode)) return 0;
        size = (uint64_t) lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
    }
    return size;
}

static int read_mbr(int fd, mbr_t *mbr) {
    lseek(fd, 0, SEEK_SET);
    ssize_t r = read(fd, mbr, MBR_SIZE);
    if (r != MBR_SIZE) return -1;
    if (mbr->sig != MBR_SIG) {
        fprintf(stderr, "%s: warning: no MBR signature (0x%04X)\n", kx_prog, mbr->sig);
    }
    return 0;
}

static int write_mbr(int fd, const mbr_t *mbr) {
    mbr_t tmp = *mbr;
    tmp.sig = MBR_SIG;
    lseek(fd, 0, SEEK_SET);
    ssize_t w = write(fd, &tmp, MBR_SIZE);
    fsync(fd);
    return (w == MBR_SIZE) ? 0 : -1;
}

static void print_table(const mbr_t *mbr, uint64_t disk_sectors) {
    printf("Device     Boot   Start       Sectors  Size (MiB)  Type\n");
    for (int i = 0; i < PART_ENTRIES; i++) {
        const part_entry_t *p = &mbr->parts[i];
        if (p->type == 0) continue;
        printf("/dev/sdX%c  %c      %-9u %-11u %-10u  %s\n", 'a' + i,
               (p->status == 0x80) ? '*' : ' ', p->lba_first, p->sectors,
               (uint32_t) ((uint64_t) p->sectors * BLOCK_SIZE / (1024 * 1024)),
               part_type_name(p->type));
    }
    uint64_t total_mb = (uint64_t) disk_sectors * BLOCK_SIZE / (1024 * 1024);
    printf("\nDisk: %lu MiB, %lu bytes, %lu sectors\n", (unsigned long) total_mb,
           (unsigned long) (disk_sectors * BLOCK_SIZE), (unsigned long) disk_sectors);
}

static uint32_t next_free_sector(const mbr_t *mbr) {
    uint32_t max_end = 2048;
    for (int i = 0; i < PART_ENTRIES; i++) {
        const part_entry_t *p = &mbr->parts[i];
        if (p->type == 0) continue;
        uint32_t end = p->lba_first + p->sectors;
        if (end > max_end) max_end = end;
    }
    return max_end;
}

static int find_empty(const mbr_t *mbr) {
    for (int i = 0; i < PART_ENTRIES; i++)
        if (mbr->parts[i].type == 0) return i;
    return -1;
}

static void help(void) {
    printf("Commands:\n");
    printf("  p   print partition table\n");
    printf("  n   add a new partition\n");
    printf("  d   delete a partition\n");
    printf("  t   change partition type\n");
    printf("  w   write table to disk and exit\n");
    printf("  q   quit without saving\n");
    printf("  h   this help\n");
}

static void interactive(int fd, mbr_t *mbr, uint64_t disk_sectors) {
    char line[128];
    int dirty = 0;
    while (1) {
        printf("fdisk> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == 0) continue;
        char cmd = *p;
        p++;
        while (*p == ' ') p++;

        switch (cmd) {
        case 'p':
            print_table(mbr, disk_sectors);
            break;
        case 'n': {
            int idx = find_empty(mbr);
            if (idx < 0) {
                printf("no empty partition slot\n");
                break;
            }
            part_entry_t *e = &mbr->parts[idx];
            printf("Partition type (linux, swap, fat32, ntfs, or hex) [linux]: ");
            fflush(stdout);
            char type_buf[32] = "linux";
            fgets(type_buf, sizeof(type_buf), stdin);
            type_buf[strcspn(type_buf, "\n")] = 0;
            if (type_buf[0] == 0) strcpy(type_buf, "linux");
            int t = parse_type(type_buf);
            if (t < 0) {
                printf("unknown type\n");
                break;
            }

            uint32_t start = next_free_sector(mbr);
            printf("First sector [%u]: ", start);
            fflush(stdout);
            char start_buf[32];
            snprintf(start_buf, sizeof(start_buf), "%u", start);
            fgets(start_buf, sizeof(start_buf), stdin);
            start_buf[strcspn(start_buf, "\n")] = 0;
            if (start_buf[0] != 0) start = strtoul(start_buf, NULL, 10);

            uint32_t max_sectors = disk_sectors - start;
            uint32_t default_sectors = max_sectors;
            printf("Size in sectors or [KMG] [%u]: ", default_sectors);
            fflush(stdout);
            char size_buf[64];
            snprintf(size_buf, sizeof(size_buf), "%u", default_sectors);
            fgets(size_buf, sizeof(size_buf), stdin);
            size_buf[strcspn(size_buf, "\n")] = 0;
            uint32_t sectors = default_sectors;
            if (size_buf[0] != 0) {
                char *end;
                sectors = strtoul(size_buf, &end, 10);
                if (*end == 'M' || *end == 'm')
                    sectors = sectors * 1024 * 1024 / BLOCK_SIZE;
                else if (*end == 'G' || *end == 'g')
                    sectors = sectors * 1024UL * 1024 * 1024 / BLOCK_SIZE;
                else if (*end == 'K' || *end == 'k')
                    sectors = sectors * 1024 / BLOCK_SIZE;
            }
            if (sectors > max_sectors) {
                printf("reducing to %u sectors (max available)\n", max_sectors);
                sectors = max_sectors;
            }
            memset(e, 0, sizeof(*e));
            e->status = 0;
            e->type = (uint8_t) t;
            e->lba_first = start;
            e->sectors = sectors;
            dirty = 1;
            printf("partition %d: %s, start=%u, size=%u\n", idx + 1, part_type_name(t), start,
                   sectors);
            break;
        }
        case 'd': {
            printf("partition number (1-%d): ", PART_ENTRIES);
            fflush(stdout);
            char num_buf[8];
            fgets(num_buf, sizeof(num_buf), stdin);
            int n = atoi(num_buf);
            if (n < 1 || n > PART_ENTRIES) {
                printf("invalid\n");
                break;
            }
            if (mbr->parts[n - 1].type == 0) {
                printf("empty slot\n");
                break;
            }
            memset(&mbr->parts[n - 1], 0, sizeof(part_entry_t));
            dirty = 1;
            printf("partition %d deleted\n", n);
            break;
        }
        case 't': {
            printf("partition number (1-%d): ", PART_ENTRIES);
            fflush(stdout);
            char num_buf[8];
            fgets(num_buf, sizeof(num_buf), stdin);
            int n = atoi(num_buf);
            if (n < 1 || n > PART_ENTRIES) {
                printf("invalid\n");
                break;
            }
            if (mbr->parts[n - 1].type == 0) {
                printf("empty slot\n");
                break;
            }
            printf("current type: %s (0x%02X)\n", part_type_name(mbr->parts[n - 1].type),
                   mbr->parts[n - 1].type);
            printf("new type: ");
            fflush(stdout);
            char type_buf[32];
            fgets(type_buf, sizeof(type_buf), stdin);
            type_buf[strcspn(type_buf, "\n")] = 0;
            int t = parse_type(type_buf);
            if (t < 0) {
                printf("unknown type\n");
                break;
            }
            mbr->parts[n - 1].type = (uint8_t) t;
            dirty = 1;
            printf("type changed to %s\n", part_type_name(t));
            break;
        }
        case 'w':
            if (write_mbr(fd, mbr) < 0) {
                kx_warn("write");
            } else {
                printf("partition table written\n");
                dirty = 0;
            }
            break;
        case 'q':
            if (dirty) printf("warning: unsaved changes\n");
            return;
        case 'h':
            help();
            break;
        default:
            printf("unknown command: '%c', try 'h'\n", cmd);
        }
    }
}

static void usage(void) {
    fprintf(stderr, "usage: %s [-l] [-w] [-t type] [-n] [-d N] device\n", kx_prog);
    fprintf(stderr, "  -l        list partition table\n");
    fprintf(stderr, "  -w        write changes (non-interactive)\n");
    fprintf(stderr, "  -t TYPE   partition type for -n (linux, fat32, swap, ...)\n");
    fprintf(stderr, "  -n        create one partition (non-interactive)\n");
    fprintf(stderr, "  -d N      delete partition N (non-interactive)\n");
    fprintf(stderr, "  (no args) interactive mode\n");
}

int main(int argc, char **argv) {
    kx_prog = kx_base(argv[0]);
    int flag_list = 0, flag_write = 0, flag_new = 0;
    int del_part = -1;
    const char *new_type = "linux";
    int ai = 1;

    while (ai < argc && argv[ai][0] == '-') {
        if (strcmp(argv[ai], "-l") == 0)
            flag_list = 1;
        else if (strcmp(argv[ai], "-w") == 0)
            flag_write = 1;
        else if (strcmp(argv[ai], "-n") == 0)
            flag_new = 1;
        else if (strcmp(argv[ai], "-d") == 0 && ai + 1 < argc) {
            del_part = atoi(argv[++ai]) - 1;
        } else if (strcmp(argv[ai], "-t") == 0 && ai + 1 < argc) {
            new_type = argv[++ai];
        } else if (strcmp(argv[ai], "--help") == 0) {
            usage();
            return 0;
        } else {
            usage();
            return 1;
        }
        ai++;
    }

    if (ai >= argc) {
        usage();
        return 1;
    }
    const char *devpath = argv[ai];

    int fd = open(devpath, O_RDWR);
    if (fd < 0) {
        kx_warn(devpath);
        return 1;
    }

    uint64_t disk_size = get_disk_size(fd);
    uint64_t disk_sectors = disk_size / BLOCK_SIZE;
    if (disk_sectors == 0) {
        fprintf(stderr, "%s: cannot determine disk size\n", kx_prog);
        close(fd);
        return 1;
    }

    mbr_t mbr;
    memset(&mbr, 0, sizeof(mbr));
    if (read_mbr(fd, &mbr) < 0) {
        kx_warn("read MBR");
        close(fd);
        return 1;
    }

    if (flag_list) {
        print_table(&mbr, disk_sectors);
        close(fd);
        return 0;
    }

    if (del_part >= 0 && del_part < PART_ENTRIES) {
        if (mbr.parts[del_part].type == 0) {
            fprintf(stderr, "%s: partition %d is empty\n", kx_prog, del_part + 1);
        } else {
            memset(&mbr.parts[del_part], 0, sizeof(part_entry_t));
            printf("partition %d deleted\n", del_part + 1);
            flag_write = 1;
        }
    }

    if (flag_new) {
        int idx = find_empty(&mbr);
        if (idx < 0) {
            fprintf(stderr, "%s: no empty partition slot\n", kx_prog);
        } else {
            int t = parse_type(new_type);
            if (t < 0) {
                fprintf(stderr, "%s: unknown type: %s\n", kx_prog, new_type);
                close(fd);
                return 1;
            }
            part_entry_t *e = &mbr.parts[idx];
            uint32_t start = next_free_sector(&mbr);
            uint32_t max_sectors = disk_sectors - start;
            memset(e, 0, sizeof(*e));
            e->type = (uint8_t) t;
            e->lba_first = start;
            e->sectors = max_sectors;
            printf("partition %d: %s, start=%u, size=%u\n", idx + 1, part_type_name(t), start,
                   max_sectors);
            flag_write = 1;
        }
    }

    if (flag_write) {
        if (write_mbr(fd, &mbr) < 0) {
            kx_warn("write MBR");
            close(fd);
            return 1;
        }
        printf("partition table written to %s\n", devpath);
        close(fd);
        return 0;
    }

    if (!flag_list) { interactive(fd, &mbr, disk_sectors); }

    close(fd);
    return 0;
}

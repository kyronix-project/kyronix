#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_SIZE 4096u
#define BLOCKS_PER_GROUP (BLOCK_SIZE * 8u)
#define INODE_SIZE 128u
#define INODES_PER_GROUP 2048u
#define EXT2_MAGIC 0xEF53u
#define EXT2_VALID_FS 1u
#define EXT2_ERRORS_CONTINUE 1u
#define EXT2_DYNAMIC_REV 1u
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define BLKGETSIZE64 0x80081272u
#define BLKGETSIZE64_COMPAT 0x1262u

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
    char s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t s_padding[820];
} ext2_super_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t bg_reserved[12];
} ext2_group_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t i_osd2[12];
} ext2_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
} ext2_dirent_t;

_Static_assert(sizeof(ext2_super_t) == 1024, "ext2 superblock size");
_Static_assert(sizeof(ext2_group_t) == 32, "ext2 group descriptor size");
_Static_assert(sizeof(ext2_inode_t) == 128, "ext2 inode size");

static const char *program;

static void die(const char *what) {
    fprintf(stderr, "%s: %s: %s\n", program, what, strerror(errno));
    exit(1);
}

static uint64_t device_size(int fd) {
    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) == 0 ||
        ioctl(fd, BLKGETSIZE64_COMPAT, &size) == 0)
        return size;

    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) return 0;
    if (lseek(fd, 0, SEEK_SET) < 0) return 0;
    return (uint64_t) end;
}

static void write_exact(int fd, uint64_t offset, const void *data, size_t size) {
    if (lseek(fd, (off_t) offset, SEEK_SET) < 0) die("seek");
    const uint8_t *p = data;
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, p + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr,
                    "%s: write failed at offset %llu (%zu/%zu bytes): %s\n",
                    program, (unsigned long long) (offset + done),
                    size - done, size, strerror(errno));
            exit(1);
        }
        if (n == 0) {
            errno = EIO;
            die("short write");
        }
        done += (size_t) n;
    }
}

static void write_block(int fd, uint32_t block, const void *data) {
    write_exact(fd, (uint64_t) block * BLOCK_SIZE, data, BLOCK_SIZE);
}

static void set_bit(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8u] |= (uint8_t) (1u << (bit % 8u));
}

static uint32_t group_blocks(uint32_t total, uint32_t group) {
    uint32_t start = group * BLOCKS_PER_GROUP;
    uint32_t left = total - start;
    return left < BLOCKS_PER_GROUP ? left : BLOCKS_PER_GROUP;
}

static void make_uuid(uint8_t uuid[16]) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, uuid, 16);
        close(fd);
        if (n == 16) goto finish;
    }
    uint64_t seed = (uint64_t) time(NULL) ^ (uint64_t) getpid();
    for (int i = 0; i < 16; i++) {
        seed = seed * 6364136223846793005ULL + 1;
        uuid[i] = (uint8_t) (seed >> 32);
    }
finish:
    uuid[6] = (uuid[6] & 0x0Fu) | 0x40u;
    uuid[8] = (uuid[8] & 0x3Fu) | 0x80u;
}

static void usage(void) {
    fprintf(stderr, "usage: %s [-F] [-L label] device\n", program);
    exit(2);
}

int main(int argc, char **argv) {
    program = argv[0];
    const char *label = "kyronix";
    const char *device = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-F") == 0) continue;
        if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            label = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') usage();
        if (device) usage();
        device = argv[i];
    }
    if (!device) usage();

    int fd = open(device, O_RDWR);
    if (fd < 0) die(device);

    uint64_t bytes = device_size(fd);
    uint64_t block_count64 = bytes / BLOCK_SIZE;
    if (block_count64 < 2048u || block_count64 > UINT32_MAX) {
        fprintf(stderr, "%s: unsupported filesystem size: %llu bytes\n",
                program, (unsigned long long) bytes);
        close(fd);
        return 1;
    }

    uint32_t blocks = (uint32_t) block_count64;
    uint32_t groups =
        (blocks + BLOCKS_PER_GROUP - 1u) / BLOCKS_PER_GROUP;
    uint32_t gdt_blocks =
        (groups * sizeof(ext2_group_t) + BLOCK_SIZE - 1u) / BLOCK_SIZE;
    uint32_t inode_table_blocks =
        (INODES_PER_GROUP * INODE_SIZE + BLOCK_SIZE - 1u) / BLOCK_SIZE;
    uint32_t metadata_blocks = 3u + gdt_blocks + inode_table_blocks;

    for (uint32_t g = 0; g < groups; g++) {
        if (group_blocks(blocks, g) <= metadata_blocks + (g == 0 ? 1u : 0u)) {
            fprintf(stderr, "%s: final block group is too small\n", program);
            close(fd);
            return 1;
        }
    }

    ext2_group_t *gdt = calloc(groups, sizeof(*gdt));
    if (!gdt) die("group descriptors");

    uint32_t free_blocks_total = 0;
    uint32_t free_inodes_total = 0;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t start = g * BLOCKS_PER_GROUP;
        uint32_t used = metadata_blocks + (g == 0 ? 1u : 0u);
        uint32_t available = group_blocks(blocks, g);

        gdt[g].bg_block_bitmap = start + 1u + gdt_blocks;
        gdt[g].bg_inode_bitmap = start + 2u + gdt_blocks;
        gdt[g].bg_inode_table = start + 3u + gdt_blocks;
        gdt[g].bg_free_blocks_count = (uint16_t) (available - used);
        gdt[g].bg_free_inodes_count =
            (uint16_t) (INODES_PER_GROUP - (g == 0 ? 10u : 0u));
        gdt[g].bg_used_dirs_count = (uint16_t) (g == 0 ? 1u : 0u);
        free_blocks_total += gdt[g].bg_free_blocks_count;
        free_inodes_total += gdt[g].bg_free_inodes_count;
    }

    ext2_super_t super;
    memset(&super, 0, sizeof(super));
    super.s_inodes_count = groups * INODES_PER_GROUP;
    super.s_blocks_count = blocks;
    super.s_r_blocks_count = blocks / 20u;
    super.s_free_blocks_count = free_blocks_total;
    super.s_free_inodes_count = free_inodes_total;
    super.s_first_data_block = 0;
    super.s_log_block_size = 2;
    super.s_log_frag_size = 2;
    super.s_blocks_per_group = BLOCKS_PER_GROUP;
    super.s_frags_per_group = BLOCKS_PER_GROUP;
    super.s_inodes_per_group = INODES_PER_GROUP;
    super.s_wtime = (uint32_t) time(NULL);
    super.s_mnt_count = 0;
    super.s_max_mnt_count = (uint16_t) -1;
    super.s_magic = EXT2_MAGIC;
    super.s_state = EXT2_VALID_FS;
    super.s_errors = EXT2_ERRORS_CONTINUE;
    super.s_lastcheck = super.s_wtime;
    super.s_creator_os = 0;
    super.s_rev_level = EXT2_DYNAMIC_REV;
    super.s_first_ino = 11;
    super.s_inode_size = INODE_SIZE;
    super.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE;
    make_uuid(super.s_uuid);
    strncpy(super.s_volume_name, label, sizeof(super.s_volume_name));

    uint8_t *zero = calloc(1, BLOCK_SIZE);
    uint8_t *block_bitmap = calloc(1, BLOCK_SIZE);
    uint8_t *inode_bitmap = calloc(1, BLOCK_SIZE);
    uint8_t *gdt_image = calloc(gdt_blocks, BLOCK_SIZE);
    uint8_t *backup_image = calloc(1, BLOCK_SIZE);
    if (!zero || !block_bitmap || !inode_bitmap || !gdt_image ||
        !backup_image)
        die("format buffers");
    memcpy(gdt_image, gdt, groups * sizeof(*gdt));

    for (uint32_t g = 0; g < groups; g++) {
        uint32_t start = g * BLOCKS_PER_GROUP;
        uint32_t available = group_blocks(blocks, g);
        uint32_t used = metadata_blocks + (g == 0 ? 1u : 0u);

        memset(block_bitmap, 0, BLOCK_SIZE);
        for (uint32_t bit = 0; bit < used; bit++) set_bit(block_bitmap, bit);
        for (uint32_t bit = available; bit < BLOCKS_PER_GROUP; bit++)
            set_bit(block_bitmap, bit);
        write_block(fd, gdt[g].bg_block_bitmap, block_bitmap);

        memset(inode_bitmap, 0, BLOCK_SIZE);
        if (g == 0)
            for (uint32_t bit = 0; bit < 10u; bit++) set_bit(inode_bitmap, bit);
        for (uint32_t bit = INODES_PER_GROUP; bit < BLOCK_SIZE * 8u; bit++)
            set_bit(inode_bitmap, bit);
        write_block(fd, gdt[g].bg_inode_bitmap, inode_bitmap);

        for (uint32_t b = 0; b < inode_table_blocks; b++)
            write_block(fd, gdt[g].bg_inode_table + b, zero);

        ext2_super_t backup = super;
        backup.s_block_group_nr = (uint16_t) g;
        if (g == 0)
            write_exact(fd, 1024, &backup, sizeof(backup));
        else {
            memset(backup_image, 0, BLOCK_SIZE);
            memcpy(backup_image, &backup, sizeof(backup));
            write_block(fd, start, backup_image);
        }

        for (uint32_t b = 0; b < gdt_blocks; b++)
            write_block(fd, start + 1u + b, gdt_image + b * BLOCK_SIZE);
    }

    uint32_t root_block = metadata_blocks;
    ext2_inode_t root;
    memset(&root, 0, sizeof(root));
    root.i_mode = 0040000u | 0755u;
    root.i_size = BLOCK_SIZE;
    root.i_atime = root.i_ctime = root.i_mtime = super.s_wtime;
    root.i_links_count = 2;
    root.i_blocks = BLOCK_SIZE / 512u;
    root.i_block[0] = root_block;
    write_exact(fd,
                (uint64_t) gdt[0].bg_inode_table * BLOCK_SIZE +
                    (2u - 1u) * INODE_SIZE,
                &root, sizeof(root));

    uint8_t *dir = calloc(1, BLOCK_SIZE);
    if (!dir) die("root directory");
    ext2_dirent_t *dot = (ext2_dirent_t *) dir;
    dot->inode = 2;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = 2;
    dir[sizeof(*dot)] = '.';
    ext2_dirent_t *dotdot = (ext2_dirent_t *) (dir + 12);
    dotdot->inode = 2;
    dotdot->rec_len = BLOCK_SIZE - 12;
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    dir[12 + sizeof(*dotdot)] = '.';
    dir[12 + sizeof(*dotdot) + 1] = '.';
    write_block(fd, root_block, dir);

    if (fsync(fd) < 0) die("sync");
    close(fd);

    printf("Created ext2 filesystem on %s: %u blocks, %u inodes, label=%.*s\n",
           device, blocks, super.s_inodes_count,
           (int) sizeof(super.s_volume_name), super.s_volume_name);

    free(dir);
    free(backup_image);
    free(gdt_image);
    free(inode_bitmap);
    free(block_bitmap);
    free(zero);
    free(gdt);
    return 0;
}

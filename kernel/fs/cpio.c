#include "cpio.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "vfs.h"

static bool hex8(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9')
            d = (uint32_t) (c - '0');
        else if (c >= 'A' && c <= 'F')
            d = (uint32_t) (c - 'A') + 10;
        else if (c >= 'a' && c <= 'f')
            d = (uint32_t) (c - 'a') + 10;
        else
            return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

typedef struct __attribute__((packed)) {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
} cpio_hdr_t;

#define CPIO_HDR_SIZE 110

static inline uint64_t align4(uint64_t x) { return (x + 3) & ~3ULL; }

static bool archive_path_safe(const char *path, size_t len) {
    if (!path || !len || path[0] == '/') return false;
    size_t pos = 0;
    while (pos < len) {
        while (pos < len && path[pos] == '/') pos++;
        size_t start = pos;
        while (pos < len && path[pos] != '/') pos++;
        size_t n = pos - start;
        if (n == 2 && path[start] == '.' && path[start + 1] == '.') return false;
    }
    return true;
}

int cpio_load(const void *data, uint64_t total_size) {
    if (!data || total_size < CPIO_HDR_SIZE) return -1;

    const uint8_t *base = (const uint8_t *) data;
    uint64_t pos = 0;
    int count = 0;

    while (pos <= total_size - CPIO_HDR_SIZE) {
        const cpio_hdr_t *hdr = (const cpio_hdr_t *) (base + pos);

        if (hdr->magic[0] != '0' || hdr->magic[1] != '7' || hdr->magic[2] != '0' ||
            hdr->magic[3] != '7' || hdr->magic[4] != '0' ||
            (hdr->magic[5] != '1' && hdr->magic[5] != '2')) {
            log_error("CPIO: bad magic at offset %lu", pos);
            return -1;
        }

        uint32_t namesize, filesize, mode, uid, gid;
        if (!hex8(hdr->namesize, &namesize) || !hex8(hdr->filesize, &filesize) ||
            !hex8(hdr->mode, &mode) || !hex8(hdr->uid, &uid) || !hex8(hdr->gid, &gid))
            return -1;
        if (!namesize) return -1;

        uint64_t name_off = pos + CPIO_HDR_SIZE;
        if (namesize > total_size - name_off) return -1;

        const char *name = (const char *) (base + name_off);
        if (name[namesize - 1] != '\0') return -1;

        uint64_t record_prefix = align4((uint64_t) CPIO_HDR_SIZE + namesize);
        if (record_prefix > total_size - pos) return -1;
        uint64_t data_off = pos + record_prefix;
        if (filesize > total_size - data_off) return -1;

        uint64_t data_end = data_off + filesize;
        if (data_end > UINT64_MAX - 3) return -1;
        uint64_t next_pos = align4(data_end);
        if (next_pos <= pos) return -1;

        if (namesize >= 10 && memcmp(name, "TRAILER!!!", 10) == 0) break;

        if (namesize <= 1 || (namesize == 2 && name[0] == '.')) goto next;

        char fullpath[512];
        const char *path = name;
        size_t path_capacity = namesize;
        if (path_capacity >= 2 && path[0] == '.' && path[1] == '/') {
            path += 2;
            path_capacity -= 2;
        }
        size_t path_len = strnlen(path, path_capacity);
        if (!archive_path_safe(path, path_len) || path_len > sizeof(fullpath) - 2) return -1;
        if (path[0] == '/') {
            strncpy(fullpath, path, sizeof(fullpath) - 1);
        } else {
            fullpath[0] = '/';
            strncpy(fullpath + 1, path, sizeof(fullpath) - 2);
        }
        fullpath[sizeof(fullpath) - 1] = '\0';

        uint32_t ftype = mode & S_IFMT;

        if (ftype == S_IFDIR) {
            vfs_node_t *n = vfs_mkdir_p(fullpath, mode & 07777);
            if (n) {
                n->uid = uid;
                n->gid = gid;
            }
        } else if (ftype == S_IFREG || ftype == 0) {
            vfs_node_t *n = vfs_create_file(fullpath, mode & 07777, base + data_off, filesize);
            if (n) {
                n->uid = uid;
                n->gid = gid;
                count++;
            }
        } else if (ftype == S_IFLNK) {
            if (filesize > 0 && filesize < 512) {
                char target[512];
                memcpy(target, base + data_off, filesize);
                target[filesize] = '\0';
                vfs_node_t *n = vfs_create_symlink(fullpath, target);
                if (n) {
                    n->uid = uid;
                    n->gid = gid;
                }
            }
        }

    next:
        pos = next_pos;
    }

    log_info("CPIO: loaded %d files into ramfs", count);
    return 0;
}

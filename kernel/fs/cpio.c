#include "cpio.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "vfs.h"

static uint32_t hex8(const char* s)
{
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
            break;
        v = (v << 4) | d;
    }
    return v;
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

static inline uint64_t align4(uint64_t x)
{
    return (x + 3) & ~3ULL;
}

int cpio_load(const void* data, uint64_t total_size)
{
    if (!data || total_size < CPIO_HDR_SIZE)
        return -1;

    const uint8_t* base = (const uint8_t*) data;
    uint64_t pos = 0;
    int count = 0;

    while (pos + CPIO_HDR_SIZE <= total_size) {
        const cpio_hdr_t* hdr = (const cpio_hdr_t*) (base + pos);

        if (hdr->magic[0] != '0' || hdr->magic[1] != '7' || hdr->magic[2] != '0' ||
            hdr->magic[3] != '7' || hdr->magic[4] != '0' ||
            (hdr->magic[5] != '1' && hdr->magic[5] != '2')) {
            log_error("CPIO: bad magic at offset %lu", pos);
            return -1;
        }

        uint32_t namesize = hex8(hdr->namesize);
        uint32_t filesize = hex8(hdr->filesize);
        uint32_t mode = hex8(hdr->mode);
        uint32_t uid = hex8(hdr->uid);
        uint32_t gid = hex8(hdr->gid);

        uint64_t name_off = pos + CPIO_HDR_SIZE;
        if (name_off + namesize > total_size)
            break;

        const char* name = (const char*) (base + name_off);

        /* data offdet: align (CPIO_HDR_SIZE + namesize) to 4 */
        uint64_t data_off = pos + align4((uint64_t) CPIO_HDR_SIZE + namesize);
        if (data_off + filesize > total_size && filesize > 0)
            break;

        /* next record offset: align data end to 4 */
        uint64_t next_pos = align4(data_off + filesize);

        if (namesize >= 10 && memcmp(name, "TRAILER!!!", 10) == 0)
            break;

        if (namesize <= 1 || (namesize == 2 && name[0] == '.'))
            goto next;

        const char* path = name;
        if (path[0] == '.' && path[1] == '/')
            path += 2;

        char fullpath[512];
        if (path[0] == '/') {
            strncpy(fullpath, path, sizeof(fullpath) - 1);
        } else {
            fullpath[0] = '/';
            strncpy(fullpath + 1, path, sizeof(fullpath) - 2);
        }
        fullpath[sizeof(fullpath) - 1] = '\0';

        uint32_t ftype = mode & S_IFMT;

        if (ftype == S_IFDIR) {
            vfs_node_t* n = vfs_mkdir_p(fullpath, mode & 07777);
            if (n) {
                n->uid = uid;
                n->gid = gid;
            }
        } else if (ftype == S_IFREG || ftype == 0) {
            vfs_node_t* n = vfs_create_file(fullpath, mode & 07777, base + data_off, filesize);
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
                vfs_node_t* n = vfs_create_symlink(fullpath, target);
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

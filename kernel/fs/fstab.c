#include "fstab.h"
#include "../drivers/block/block.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "vfs.h"
#include "vfs_internal.h"

#define FSTAB_LINE_MAX 256
#define FSTAB_ENTRIES_MAX 32

struct fstab_entry {
    char device[64];
    char mount_point[64];
    char fstype[16];
};

static int parse_fstab(const char *path, struct fstab_entry *entries, int max) {
    vfs_node_t *n = vfs_lookup(path);
    if (!n || n->type != VFS_TYPE_REG) {
        if (n) vfs_node_unref_internal(n);
        return 0;
    }
    if (!n->data && n->fs_ops && n->fs_ops->read) n->fs_ops->read(n, NULL, 0, n->size);
    if (!n->data) {
        vfs_node_unref_internal(n);
        return 0;
    }

    char *text = (char *) n->data;
    uint64_t len = n->size;
    int count = 0;
    uint64_t pos = 0;

    while (pos < len && count < max) {
        char line[FSTAB_LINE_MAX];
        int li = 0;
        while (pos < len && text[pos] != '\n' && li < FSTAB_LINE_MAX - 1) {
            if (text[pos] != '\r') line[li++] = text[pos];
            pos++;
        }
        if (pos < len && text[pos] == '\n') pos++;
        line[li] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        char *fields[4];
        int nf = 0;
        char *tok = p;
        while (nf < 4) {
            while (*tok == ' ' || *tok == '\t') tok++;
            if (*tok == '\0' || *tok == '#') break;
            fields[nf++] = tok;
            while (*tok && *tok != ' ' && *tok != '\t' && *tok != '#') tok++;
            if (*tok) *tok++ = '\0';
        }
        if (nf < 2) continue;

        memset(&entries[count], 0, sizeof(struct fstab_entry));
        strncpy(entries[count].device, fields[0], sizeof(entries[count].device) - 1);
        strncpy(entries[count].mount_point, fields[1], sizeof(entries[count].mount_point) - 1);
        if (nf >= 3) strncpy(entries[count].fstype, fields[2], sizeof(entries[count].fstype) - 1);
        count++;
    }

    vfs_node_unref_internal(n);
    return count;
}

bool fstab_mount_all(const char *fstab_path) {
    struct fstab_entry entries[FSTAB_ENTRIES_MAX];
    int count = parse_fstab(fstab_path, entries, FSTAB_ENTRIES_MAX);
    if (count <= 0) return false;

    for (int i = 0; i < count; i++) {
        struct fstab_entry *e = &entries[i];
        bool skip = false;
        if (strcmp(e->mount_point, "/") == 0) skip = true;
        if (strcmp(e->mount_point, "none") == 0) skip = true;
        if (skip) continue;

        struct block_device *bd = block_by_name(e->device);
        if (!bd) {
            log_warn("fstab: device '%s' not found", e->device);
            continue;
        }

        bool ok = false;
        struct filesystem *fs = vfs_find_fs(e->fstype);
        if (!fs && e->fstype[0] != '\0') {
            log_warn("fstab: unsupported fstype '%s' for %s", e->fstype, e->device);
            continue;
        }
        if (fs) {
            ok = fs->mount(bd, e->mount_point);
        } else {
            for (int f = 0; f < vfs_fs_count() && !ok; f++) {
                struct filesystem *candidate = vfs_get_fs(f);
                if (candidate->check_root && candidate->check_root(bd))
                    ok = candidate->mount(bd, e->mount_point);
            }
        }

        if (ok) {
            log_info("fstab: mounted %s at %s", e->device, e->mount_point);
        } else {
            log_warn("fstab: failed to mount %s at %s", e->device, e->mount_point);
        }
    }

    return count > 0;
}

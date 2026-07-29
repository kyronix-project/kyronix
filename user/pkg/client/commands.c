#define _GNU_SOURCE

#include "commands.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http.h"
#include "md5.h"
#include "util.h"

int verbose_mode = 0;
int yes_mode = 0;
static char g_active_tmp[512];

static void cleanup_active_tmp(void) {
    if (g_active_tmp[0]) {
        remove_tree(g_active_tmp);
        g_active_tmp[0] = '\0';
    }
}

static int extract_json_string(const char *json, const char *key, char *out, size_t n) {
    if (!json || !key || !out || n == 0) return -1;
    char needle[128];
    int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_len < 0 || (size_t) needle_len >= sizeof(needle)) return -1;
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    size_t used = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char) *p++;
        if (c < 0x20) return -1;
        if (c == '\\') {
            char escaped = *p++;
            if (!escaped) return -1;
            switch (escaped) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: return -1;
            }
        }
        if (used + 1 >= n) return -1;
        out[used++] = (char) c;
    }
    if (*p != '"') return -1;
    out[used] = '\0';
    return 0;
}

static int parse_depends(const char *json, PackageInfo *pkg) {
    pkg->depends_count = 0;
    const char *p = strstr(json, "\"depends\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    while (*p && *p != ']') {
        while (*p && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r')) p++;
        if (*p == '"' ) {
            if (pkg->depends_count >= MAX_DEPS) return -1;
            p++;
            const char *end = strchr(p, '"');
            if (!end) break;
            size_t len = (size_t)(end - p);
            if (!len || len >= 128 || memchr(p, '\\', len)) return -1;
            memcpy(pkg->depends[pkg->depends_count], p, len);
            pkg->depends[pkg->depends_count][len] = '\0';
            if (!valid_pkg_name(pkg->depends[pkg->depends_count])) return -1;
            pkg->depends_count++;
            p = end + 1;
        } else {
            break;
        }
    }
    return *p == ']' ? 0 : -1;
}

static int safe_manifest_text(const char *text) {
    if (!text) return 0;
    for (const unsigned char *p = (const unsigned char *) text; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

static int parse_manifest(const char *json, PackageInfo *pkg) {
    memset(pkg, 0, sizeof(*pkg));
    if (extract_json_string(json, "name", pkg->name, sizeof(pkg->name)) != 0) return -1;
    if (extract_json_string(json, "version", pkg->version, sizeof(pkg->version)) != 0) return -1;
    if (extract_json_string(json, "description", pkg->description,
                            sizeof(pkg->description)) != 0)
        pkg->description[0] = '\0';
    if (extract_json_string(json, "arch", pkg->arch, sizeof(pkg->arch)) != 0) return -1;
    if (extract_json_string(json, "maintainer", pkg->maintainer,
                            sizeof(pkg->maintainer)) != 0)
        pkg->maintainer[0] = '\0';
    if (extract_json_string(json, "license", pkg->license, sizeof(pkg->license)) != 0)
        pkg->license[0] = '\0';
    if (extract_json_string(json, "homepage", pkg->homepage, sizeof(pkg->homepage)) != 0)
        pkg->homepage[0] = '\0';

    if (!valid_pkg_name(pkg->name) || !safe_manifest_text(pkg->version) ||
        !pkg->version[0] || !safe_manifest_text(pkg->description) ||
        !safe_manifest_text(pkg->arch) || !safe_manifest_text(pkg->maintainer) ||
        !safe_manifest_text(pkg->license) || !safe_manifest_text(pkg->homepage))
        return -1;

    const char *rev = strstr(json, "\"revision\"");
    if (rev) {
        rev += 10;
        while (*rev && (*rev == ' ' || *rev == ':')) rev++;
        int val = 0;
        int neg = 1;
        if (*rev == '-') { neg = -1; rev++; }
        if (!isdigit((unsigned char) *rev)) return -1;
        while (*rev >= '0' && *rev <= '9') {
            if (val > (INT_MAX - (*rev - '0')) / 10) return -1;
            val = val * 10 + (*rev - '0');
            rev++;
        }
        pkg->revision = val * neg;
    }

    return parse_depends(json, pkg);
}

static int verify_checksum(const char *archive_path, const char *checksum_path) {
    size_t len = 0;
    char *txt = read_file(checksum_path, &len);
    if (!txt) return -1;
    trim_crlf(txt);

    char expected[33] = "";
    if (txt[0] == '{') {
        if (extract_json_string(txt, "checksum", expected, sizeof(expected)) != 0) {
            free(txt);
            return -1;
        }
    } else {
        if (strlen(txt) != 32) {
            free(txt);
            return -1;
        }
        memcpy(expected, txt, 33);
    }
    if (strlen(expected) != 32) {
        free(txt);
        return -1;
    }
    for (int i = 0; i < 32; i++)
        if (!isxdigit((unsigned char) expected[i])) {
            free(txt);
            return -1;
        }

    char actual[33];
    if (md5_file_hex(archive_path, actual) != 0) {
        free(txt);
        return -1;
    }

    unsigned mismatch = 0;
    for (int i = 0; i < 32; i++)
        mismatch |= (unsigned) (tolower((unsigned char) expected[i]) ^
                               (unsigned char) actual[i]);
    free(txt);
    return mismatch == 0 ? 0 : -1;
}

static int is_installed(const char *name) {
    char path[512];
    if (!valid_pkg_name(name)) return 0;
    snprintf(path, sizeof(path), "%s/installed/%s/manifest", PKG_STATE_DIR, name);
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int get_installed_version(const char *name, char *ver, size_t ver_sz) {
    char path[512];
    if (!valid_pkg_name(name)) return -1;
    snprintf(path, sizeof(path), "%s/installed/%s/manifest", PKG_STATE_DIR, name);
    size_t len = 0;
    char *txt = read_file(path, &len);
    if (!txt) return -1;
    ver[0] = '\0';
    char *line = txt;
    while (*line) {
        if (strncmp(line, "version=", 8) == 0) {
            const char *val = line + 8;
            while (*val == ' ' || *val == '\t') val++;
            size_t vlen = 0;
            while (val[vlen] && val[vlen] != '\n' && val[vlen] != '\r' && val[vlen] != ' ') vlen++;
            if (vlen > 63) vlen = 63;
            memcpy(ver, val, vlen);
            ver[vlen] = '\0';
            break;
        }
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }
    free(txt);
    if (ver_sz > 0) ver[ver_sz - 1] = '\0';
    return ver[0] ? 0 : -1;
}

/*
 * Manifest helpers for reverse dependency tracking.
 *
 * Local manifest format (/var/lib/pkg/installed/{name}/manifest):
 *   name=... version=... description=... arch=... install_dir=...
 *   depends=pkg1,pkg2           ← forward: what this package needs
 *   required_by=pkg3,pkg4       ← reverse: who needs this package
 *
 * Both fields are comma-separated (no spaces around commas).
 * An empty value means no dependencies / no dependents.
 */

static void local_manifest_path(char *out, size_t n, const char *name) {
    if (!valid_pkg_name(name)) dief("invalid package name in local registry");
    snprintf(out, n, "%s/installed/%s/manifest", PKG_STATE_DIR, name);
}

/* Generic field reader: reads key=value from manifest, writes value into out.
 * Returns 0 on success, -1 if key not found. */
static int manifest_read_field(const char *name, const char *key, char *out, size_t out_sz) {
    char path[1024];
    local_manifest_path(path, sizeof(path), name);
    size_t len = 0;
    char *txt = read_file(path, &len);
    if (!txt) return -1;

    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    out[0] = '\0';
    char *line = txt;
    while (*line) {
        if (strncmp(line, needle, strlen(needle)) == 0) {
            const char *val = line + strlen(needle);
            size_t vlen = 0;
            while (val[vlen] && val[vlen] != '\n' && val[vlen] != '\r') vlen++;
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            free(txt);
            return 0;
        }
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }
    free(txt);
    return -1;
}

/* Read comma-separated list field into array. Returns count. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static int manifest_read_list(const char *name, const char *key, char items[][128], int max) {
    char val[4096];
    if (manifest_read_field(name, key, val, sizeof(val)) != 0) return 0;
    if (!val[0]) return 0;

    int count = 0;
    char *p = val;
    while (*p && count < max) {
        while (*p == ',') p++;
        if (!*p) break;
        char *end = strchr(p, ',');
        char *item_start = p;
        char *item_end = end ? end : p + strlen(p);
        while (item_start < item_end && *item_start == ' ') item_start++;
        while (item_end > item_start && item_end[-1] == ' ') item_end--;
        size_t nlen = (size_t) (item_end - item_start);
        if (!nlen || nlen >= 128) return 0;
        memcpy(items[count], item_start, nlen);
        items[count][nlen] = '\0';
        if (!valid_pkg_name(items[count])) return 0;
        count++;
        p = end ? end + 1 : item_end;
    }
    return count;
}

/*
 * Rewrite a manifest field in-place. Reads the whole manifest, replaces the
 * key=value line, writes it back. Creates the field if it doesn't exist.
 */
static int append_text(char *out, size_t capacity, size_t *used,
                       const char *text, size_t len) {
    if (len >= capacity - *used) return -1;
    memcpy(out + *used, text, len);
    *used += len;
    out[*used] = '\0';
    return 0;
}

static void manifest_write_field(const char *name, const char *key, const char *value) {
    if (!valid_pkg_name(name) || !key || !safe_manifest_text(key) ||
        !value || !safe_manifest_text(value))
        dief("invalid local manifest update");
    char path[1024];
    local_manifest_path(path, sizeof(path), name);

    size_t len = 0;
    char *txt = read_file(path, &len);
    size_t capacity = len + strlen(key) + strlen(value) + 4;
    if (capacity > 64U * 1024U) {
        free(txt);
        dief("local manifest is too large");
    }
    char *new_content = (char *) calloc(capacity, 1);
    if (!new_content) {
        free(txt);
        dief("out of memory");
    }
    size_t used = 0;

    char needle[64];
    int needle_len = snprintf(needle, sizeof(needle), "%s=", key);
    if (needle_len < 0 || (size_t) needle_len >= sizeof(needle)) {
        free(txt);
        free(new_content);
        dief("invalid local manifest key");
    }
    int found = 0;

    if (txt) {
        char *line = txt;
        while (*line) {
            char *eol = strchr(line, '\n');
            size_t llen = eol ? (size_t)(eol - line) : strlen(line);

            if (llen >= (size_t) needle_len &&
                strncmp(line, needle, (size_t) needle_len) == 0) {
                if (append_text(new_content, capacity, &used, needle,
                                (size_t) needle_len) != 0 ||
                    append_text(new_content, capacity, &used, value,
                                strlen(value)) != 0 ||
                    append_text(new_content, capacity, &used, "\n", 1) != 0)
                    goto too_large;
                found = 1;
            } else {
                if (append_text(new_content, capacity, &used, line, llen) != 0 ||
                    append_text(new_content, capacity, &used, "\n", 1) != 0)
                    goto too_large;
            }

            line += llen;
            if (*line == '\n') line++;
        }
        free(txt);
        txt = NULL;
    }

    if (!found) {
        if (append_text(new_content, capacity, &used, needle,
                        (size_t) needle_len) != 0 ||
            append_text(new_content, capacity, &used, value, strlen(value)) != 0 ||
            append_text(new_content, capacity, &used, "\n", 1) != 0)
            goto too_large;
    }

    if (write_text_file(path, new_content) != 0) {
        free(new_content);
        dief("failed to update local manifest");
    }
    free(new_content);
    return;

too_large:
    free(txt);
    free(new_content);
    dief("local manifest is too large");
}

/* Add a name to a comma-separated list field (no duplicates). */
static void manifest_list_add(const char *name, const char *key, const char *item) {
    char items[MAX_DEPS][128];
    int count = manifest_read_list(name, key, items, MAX_DEPS);

    /* check for duplicates */
    for (int i = 0; i < count; i++) {
        if (strcmp(items[i], item) == 0) return;
    }

    if (count >= MAX_DEPS) return;

    snprintf(items[count], sizeof(items[count]), "%s", item);
    count++;

    /* rebuild comma-separated string */
    char val[4096] = "";
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        size_t item_len = strlen(items[i]);
        size_t separator = i > 0 ? 1U : 0U;
        if (separator + item_len >= sizeof(val) - used)
            dief("local dependency list is too large");
        if (separator) val[used++] = ',';
        memcpy(val + used, items[i], item_len);
        used += item_len;
        val[used] = '\0';
    }
    manifest_write_field(name, key, val);
}

/* Remove a name from a comma-separated list field. */
static void manifest_list_remove(const char *name, const char *key, const char *item) {
    char items[MAX_DEPS][128];
    int count = manifest_read_list(name, key, items, MAX_DEPS);
    int new_count = 0;

    for (int i = 0; i < count; i++) {
        if (strcmp(items[i], item) != 0) {
            if (i != new_count) {
                snprintf(items[new_count], sizeof(items[new_count]), "%s", items[i]);
            }
            new_count++;
        }
    }

    char val[4096] = "";
    size_t used = 0;
    for (int i = 0; i < new_count; i++) {
        size_t item_len = strlen(items[i]);
        size_t separator = i > 0 ? 1U : 0U;
        if (separator + item_len >= sizeof(val) - used)
            dief("local dependency list is too large");
        if (separator) val[used++] = ',';
        memcpy(val + used, items[i], item_len);
        used += item_len;
        val[used] = '\0';
    }
    manifest_write_field(name, key, val);
}
#pragma GCC diagnostic pop



/*
 * Simple version comparison: "1.2.3" vs "1.2.4"
 * Returns -1 if a < b, 0 if equal, 1 if a > b.
 * Non-numeric segments compared lexicographically.
 * Used for future version constraint enforcement in dependency resolution.
 */
__attribute__((unused))
static int version_compare(const char *a, const char *b) {
    while (*a || *b) {
        while (*a == '0' && isdigit((unsigned char)a[1])) a++;
        while (*b == '0' && isdigit((unsigned char)b[1])) b++;

        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *sa = a, *sb = b;
            while (isdigit((unsigned char)*a)) a++;
            while (isdigit((unsigned char)*b)) b++;
            size_t la = (size_t)(a - sa), lb = (size_t)(b - sb);
            if (la != lb) return la < lb ? -1 : 1;
            int cmp = strncmp(sa, sb, la);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
            if (*a == '.' || *b == '.') {
                if (*a == '.') a++;
                if (*b == '.') b++;
            }
            continue;
        }

        if (*a == '.' && *b != '.') return -1;
        if (*b == '.' && *a != '.') return 1;
        if (*a < *b) return -1;
        if (*a > *b) return 1;
        if (*a) a++;
        if (*b) b++;
    }
    return 0;
}

static char *fetch_manifest_from_repos(const char *name, char **out_endpoint) {
    RepoConfig repos[MAX_REPOS];
    int count = read_repos(repos, MAX_REPOS);
    if (count == 0) return NULL;

    for (int i = 0; i < count; i++) {
        char url[1200];
        snprintf(url, sizeof(url), "%s/packages/%s", repos[i].url, name);

        int code = 0;
        char *manifest = http_get_body(url, &code);
        if (code == 200 && manifest) {
            *out_endpoint = strdup(repos[i].url);
            return manifest;
        }
        free(manifest);
    }
    return NULL;
}

typedef struct {
    char name[128];
    char version[64];
    char endpoint[512];
    long download_size;
    int installed;
} ResolvedPkg;

static int validate_extracted_tree(const char *path, unsigned depth,
                                   unsigned *file_count) {
    if (depth > 64) return -1;
    struct stat st;
    if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) return -1;
    if (S_ISREG(st.st_mode)) {
        if ((st.st_mode & (S_ISUID | S_ISGID)) != 0 || ++*file_count > 100000)
            return -1;
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) return -1;

    DIR *dir = opendir(path);
    if (!dir) return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        for (const unsigned char *p = (const unsigned char *) entry->d_name; *p; p++)
            if (*p < 0x20 || *p == 0x7f) result = -1;
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (result != 0 || n < 0 || (size_t) n >= sizeof(child) ||
            validate_extracted_tree(child, depth + 1, file_count) != 0) {
            result = -1;
            break;
        }
    }
    closedir(dir);
    return result;
}

static int copy_regular_file(const char *source, const char *destination,
                             mode_t mode) {
    int source_fd = open(source, O_RDONLY);
    if (source_fd < 0) {
        log_warn("cannot open source %s: %s", source, strerror(errno));
        return -1;
    }
    int destination_fd =
        open(destination, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (destination_fd < 0) {
        log_warn("cannot open destination %s: %s", destination, strerror(errno));
        close(source_fd);
        return -1;
    }

    int result = 0;
    size_t buf_size = 64U * 1024U;
    unsigned char *buffer = malloc(buf_size);
    if (!buffer) {
        log_warn("cannot allocate copy buffer");
        close(source_fd);
        close(destination_fd);
        unlink(destination);
        return -1;
    }
    for (;;) {
        ssize_t got = read(source_fd, buffer, buf_size);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            log_warn("read error during copy: %s", strerror(errno));
            result = -1;
            break;
        }
        if (got == 0) break;
        size_t offset = 0;
        while (offset < (size_t) got) {
            ssize_t written =
                write(destination_fd, buffer + offset, (size_t) got - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                log_warn("write error during copy: %s", strerror(errno));
                result = -1;
                break;
            }
            offset += (size_t) written;
        }
        if (result != 0) break;
    }
    free(buffer);
    if (close(source_fd) != 0) {
        log_warn("close source error: %s", strerror(errno));
        result = -1;
    }
    if (close(destination_fd) != 0) {
        log_warn("close destination error: %s", strerror(errno));
        result = -1;
    }
    if (result != 0) unlink(destination);
    return result;
}

static void snapshot_tree(const char *dir, FILE *out, unsigned depth) {
    if (depth > 64) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        fprintf(out, "%s\n", path);
        if (e->d_type == DT_DIR)
            snapshot_tree(path, out, depth + 1);
        else if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
                snapshot_tree(path, out, depth + 1);
        }
    }
    closedir(d);
}

static int write_snapshot(const char *roots[], int nroots, const char *outpath) {
    char sort_cmd[512];
    snprintf(sort_cmd, sizeof(sort_cmd), "sort -u -o %s", outpath);
    FILE *pipe = popen(sort_cmd, "w");
    if (!pipe) return -1;
    for (int i = 0; i < nroots; i++)
        snapshot_tree(roots[i], pipe, 0);
    return pclose(pipe);
}

static int install_payload_recursive(const char *source, const char *destination,
                                     FILE *created, unsigned depth,
                                     unsigned *file_count) {
    if (depth > 64) return -1;
    DIR *dir = opendir(source);
    if (!dir) {
        log_warn("cannot open payload dir %s: %s", source, strerror(errno));
        return -1;
    }
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char source_path[1024], destination_path[1024];
        int source_len = snprintf(source_path, sizeof(source_path), "%s/%s",
                                  source, entry->d_name);
        int destination_len =
            snprintf(destination_path, sizeof(destination_path), "%s/%s",
                     destination, entry->d_name);
        if (source_len < 0 || (size_t) source_len >= sizeof(source_path)) {
            log_warn("source path too long for %s/%s", source, entry->d_name);
            result = -1;
            break;
        }
        if (destination_len < 0 ||
            (size_t) destination_len >= sizeof(destination_path)) {
            log_warn("destination path too long for %s/%s", destination, entry->d_name);
            result = -1;
            break;
        }
        if (!safe_managed_path(destination_path)) {
            log_warn("unsafe path rejected: %s", destination_path);
            result = -1;
            break;
        }

        struct stat st;
        if (lstat(source_path, &st) != 0) {
            log_warn("cannot stat %s: %s", source_path, strerror(errno));
            result = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(destination_path, 0755) != 0 && errno != EEXIST) {
                log_warn("mkdir %s failed: %s", destination_path, strerror(errno));
                result = -1;
                break;
            }
            struct stat destination_st;
            if (lstat(destination_path, &destination_st) != 0 ||
                !S_ISDIR(destination_st.st_mode) ||
                install_payload_recursive(source_path, destination_path, created,
                                          depth + 1, file_count) != 0) {
                result = -1;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (++*file_count > 100000U) {
                log_warn("too many files");
                result = -1;
                break;
            }
            if (copy_regular_file(source_path, destination_path, 0755) != 0 ||
                fprintf(created, "%s\n", destination_path) < 0) {
                if (fprintf(created, "%s\n", destination_path) < 0)
                    log_warn("fprintf to created file failed: %s", strerror(ferror(created) ? errno : 0));
                unlink(destination_path);
                result = -1;
                break;
            }
        } else {
            log_warn("unsupported file type for %s", source_path);
            result = -1;
            break;
        }
    }
    closedir(dir);
    return result;
}

static void rollback_created_files(FILE *created) {
    if (!created) return;
    fflush(created);
    rewind(created);
    char path[1024];
    while (fgets(path, sizeof(path), created)) {
        trim_crlf(path);
        struct stat st;
        if (safe_managed_path(path) && lstat(path, &st) == 0 &&
            (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)))
            unlink(path);
    }
    rewind(created);
    while (fgets(path, sizeof(path), created)) {
        trim_crlf(path);
        rmdir(path);
    }
}

static int resolve_dependencies(const char *name, ResolvedPkg *out, int out_max,
                                char visited[][128], int *vis_count) {
    if (!valid_pkg_name(name)) dief("invalid dependency name");
    for (int i = 0; i < *vis_count; i++) {
        if (strcmp(visited[i], name) == 0) {
            dief("circular dependency detected: %s -> %s", visited[i], name);
        }
    }
    if (*vis_count >= MAX_DEP_DEPTH)
        dief("dependency depth exceeded (max %d)", MAX_DEP_DEPTH);

    snprintf(visited[*vis_count], 128, "%s", name);
    (*vis_count)++;

    for (int i = 0; i < out_max; i++) {
        if (out[i].name[0] && strcmp(out[i].name, name) == 0) {
            (*vis_count)--;
            return 0;
        }
    }

    int already_installed = is_installed(name);

    char *manifest = NULL;
    char endpoint_buf[512] = "";
    PackageInfo pkg;

    if (!already_installed) {
        char *ep = NULL;
        manifest = fetch_manifest_from_repos(name, &ep);
        if (!manifest) {
            dief("package '%s' not found in any repository", name);
        }
        if (parse_manifest(manifest, &pkg) != 0) {
            free(manifest);
            free(ep);
            dief("malformed manifest for '%s'", name);
        }
        free(manifest);

        if (strcmp(pkg.name, name) != 0) {
            free(ep);
            dief("repository returned manifest for '%s' while resolving '%s'",
                 pkg.name, name);
        }
        if (strcmp(pkg.arch, "x86-64") != 0) {
            free(ep);
            dief("architecture %s not supported", pkg.arch);
        }

        snprintf(endpoint_buf, sizeof(endpoint_buf), "%s", ep);
        free(ep);
    } else {
        char ver[64];
        get_installed_version(name, ver, sizeof(ver));
        snprintf(pkg.version, sizeof(pkg.version), "%s", ver);
        pkg.depends_count = 0;

        char *ep = NULL;
        manifest = fetch_manifest_from_repos(name, &ep);
        if (manifest) {
            PackageInfo remote_pkg;
            if (parse_manifest(manifest, &remote_pkg) == 0 &&
                strcmp(remote_pkg.name, name) == 0 &&
                strcmp(remote_pkg.arch, "x86-64") == 0) {
                pkg = remote_pkg;
            } else {
                log_warn("ignoring malformed manifest for installed package '%s'", name);
            }
            free(manifest);
        }
        free(ep);
    }

    for (int d = 0; d < pkg.depends_count; d++) {
        if (is_installed(pkg.depends[d])) {
            char have[64];
            get_installed_version(pkg.depends[d], have, sizeof(have));
            if (verbose_mode)
                log_info("dependency %s already installed (have %s)", pkg.depends[d], have);
            continue;
        }
        resolve_dependencies(pkg.depends[d], out, out_max, visited, vis_count);
    }

    if (!already_installed) {
        int slot = 0;
        while (slot < out_max && out[slot].name[0]) slot++;
        if (slot >= out_max) dief("too many packages to install (max %d)", out_max);

        snprintf(out[slot].name, sizeof(out[slot].name), "%s", pkg.name);
        snprintf(out[slot].version, sizeof(out[slot].version), "%s", pkg.version);
        snprintf(out[slot].endpoint, sizeof(out[slot].endpoint), "%s", endpoint_buf);
        out[slot].installed = 0;
    }

    (*vis_count)--;
    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static int do_install(const char *name, const char *endpoint, long download_size,
                      const char *requested_by, int explicit) {
    if (!valid_pkg_name(name) || !valid_repo_url(endpoint))
        dief("invalid resolved package");
    char url[2048];
    snprintf(url, sizeof(url), "%s/packages/%s", endpoint, name);

    fprintf(stdout, "%s=>%s resolving %s%s%s", ANSI_CYAN, ANSI_RESET, ANSI_BOLD, name, ANSI_RESET);

    int code = 0;
    char *manifest = http_get_body(url, &code);
    if (code == 403) dief("access denied by registry");
    if (code != 200) dief("package '%s' not found at %s", name, endpoint);

    PackageInfo pkg;
    if (parse_manifest(manifest, &pkg) != 0) {
        free(manifest);
        dief("malformed package manifest");
    }
    free(manifest);

    if (strcmp(pkg.name, name) != 0)
        dief("package identity mismatch: expected '%s', got '%s'", name, pkg.name);
    if (strcmp(pkg.arch, "x86-64") != 0) dief("architecture %s not supported", pkg.arch);

    fprintf(stdout, "\n  version: %s rev %d", pkg.version, pkg.revision);
    fprintf(stdout, " | arch: %s", pkg.arch);
    if (pkg.maintainer[0]) fprintf(stdout, " | maintainer: %s", pkg.maintainer);
    fprintf(stdout, "\n");
    if (pkg.license[0])    fprintf(stdout, "  license: %s", pkg.license);
    if (pkg.homepage[0])   fprintf(stdout, " | homepage: %s", pkg.homepage);
    if (pkg.license[0] || pkg.homepage[0]) fprintf(stdout, "\n");
    if (pkg.depends_count > 0) {
        fprintf(stdout, "  depends: ");
        for (int i = 0; i < pkg.depends_count; i++)
            fprintf(stdout, "%s%s", i > 0 ? ", " : "", pkg.depends[i]);
        fprintf(stdout, "\n");
    }

    char tmp_template[] = "/tmp/pkg-XXXXXX";
    char *tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) dief("failed to create temporary directory");
    if (!g_active_tmp[0]) atexit(cleanup_active_tmp);
    snprintf(g_active_tmp, sizeof(g_active_tmp), "%s", tmp_dir);

    char archive_path[512], checksum_path[512], extract_dir[512];
    snprintf(archive_path, sizeof(archive_path), "%s/%s", tmp_dir, "package.gz");
    snprintf(checksum_path, sizeof(checksum_path), "%s/%s", tmp_dir, "package.gz.md5");
    snprintf(extract_dir, sizeof(extract_dir), "%s/%s", tmp_dir, "extract");
    ensure_dir(extract_dir);

    const char *install_dir = "/usr/bin";
    char base[2048];
    snprintf(base, sizeof(base), "%s/packages/%s", endpoint, name);

    char url_archive[2200], url_checksum[2200];
    snprintf(url_archive, sizeof(url_archive), "%s/archive", base);
    snprintf(url_checksum, sizeof(url_checksum), "%s/checksum", base);

    if (download_size > 0) {
        if (download_size > 1048576)
            fprintf(stdout, "%s=>%s downloading... %ld.%ld MB", ANSI_CYAN, ANSI_RESET,
                    download_size / 1048576, (download_size % 1048576) * 10 / 1048576);
        else
            fprintf(stdout, "%s=>%s downloading... %ld KB", ANSI_CYAN, ANSI_RESET, download_size / 1024);
    } else {
        fprintf(stdout, "%s=>%s downloading...", ANSI_CYAN, ANSI_RESET);
    }
    fflush(stdout);
    if (http_download(url_archive, archive_path, "archive") != 0) dief("archive download failed");
    if (http_download(url_checksum, checksum_path, "checksum") != 0) dief("checksum download failed");

    fprintf(stdout, "%s=>%s verifying checksum...", ANSI_CYAN, ANSI_RESET);
    fflush(stdout);
    if (verify_checksum(archive_path, checksum_path) != 0) dief("checksum mismatch - archive corrupted");
    fprintf(stdout, " %sok%s\n", ANSI_GREEN, ANSI_RESET);

    fprintf(stdout, "%s=>%s extracting...", ANSI_CYAN, ANSI_RESET);
    fflush(stdout);
    char *tar_argv[] = {
        "/bin/tar", "-x", "-I", "/bin/gzip -dcf", "-f", archive_path,
        "-C", extract_dir,
        "--transform=flags=r;s|^\\.$|__skip__|",
        "--no-same-owner", "--no-same-permissions",
        NULL
    };
    if (run_cmd(tar_argv) != 0)
        dief("archive extraction failed");
    unsigned extracted_files = 0;
    if (validate_extracted_tree(extract_dir, 0, &extracted_files) != 0)
        dief("archive contains unsafe file types or paths");
    fprintf(stdout, " %sok%s\n", ANSI_GREEN, ANSI_RESET);

    ensure_dir_mode(PKG_STATE_DIR, 0700);
    char reg_parent[512];
    snprintf(reg_parent, sizeof(reg_parent), "%s/installed", PKG_STATE_DIR);
    ensure_dir_mode(reg_parent, 0700);

    char reg_dir[1024];
    snprintf(reg_dir, sizeof(reg_dir), "%s/installed/%s", PKG_STATE_DIR, name);
    if (mkdir(reg_dir, 0700) != 0)
        dief("package registry already exists or cannot be created");

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest", reg_dir);
    int manifest_fd =
        open(manifest_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    FILE *mf = manifest_fd >= 0 ? fdopen(manifest_fd, "w") : NULL;
    if (!mf) {
        if (manifest_fd >= 0) close(manifest_fd);
        remove_tree(reg_dir);
        dief("failed to create package manifest");
    }
    int manifest_ok = fprintf(mf, "name=%s\n", pkg.name) > 0 &&
                      fprintf(mf, "version=%s\n", pkg.version) > 0 &&
                      fprintf(mf, "description=%s\n", pkg.description) > 0 &&
                      fprintf(mf, "arch=%s\n", pkg.arch) > 0 &&
                      fprintf(mf, "install_dir=%s\n", install_dir) > 0 &&
                      fprintf(mf, "depends=") > 0;
    for (int i = 0; manifest_ok && i < pkg.depends_count; i++)
        if (fprintf(mf, "%s%s", i > 0 ? "," : "", pkg.depends[i]) < 0)
            manifest_ok = 0;
    if (manifest_ok && fprintf(mf, "\nrequired_by=%s\nexplicit=%d\n",
                               requested_by ? requested_by : "", explicit) < 0)
        manifest_ok = 0;
    if (fclose(mf) != 0) manifest_ok = 0;
    if (!manifest_ok) {
        remove_tree(reg_dir);
        dief("failed to save package manifest");
    }

    char payload_path[1024];
    snprintf(payload_path, sizeof(payload_path), "%s/payload", extract_dir);
    struct stat payload_st;
    if (lstat(payload_path, &payload_st) != 0 || !S_ISDIR(payload_st.st_mode)) {
        remove_tree(reg_dir);
        dief("archive does not contain a payload directory");
    }

    char created_path[1024];
    snprintf(created_path, sizeof(created_path), "%s/.created", tmp_dir);
    FILE *created = fopen(created_path, "w+");
    if (!created) {
        remove_tree(reg_dir);
        dief("failed to create install transaction");
    }
    unsigned installed_files = 0;
    char script_path[1024];
    snprintf(script_path, sizeof(script_path), "%s/install.sh", extract_dir);
    struct stat script_st;
    int has_script = (stat(script_path, &script_st) == 0 && S_ISREG(script_st.st_mode));

    if (has_script) {
        const char *roots[] = {"/usr", "/etc", "/opt", "/bin", "/sbin", "/lib", "/lib64"};
        int nroots = sizeof(roots) / sizeof(roots[0]);
        char before_path[512], after_path[512];
        snprintf(before_path, sizeof(before_path), "%s/.before", tmp_dir);
        snprintf(after_path, sizeof(after_path), "%s/.after", tmp_dir);

        if (write_snapshot(roots, nroots, before_path) != 0) {
            rollback_created_files(created);
            fclose(created);
            remove_tree(reg_dir);
            dief("failed to create pre-install snapshot");
        }
        char *sh_argv[] = {"/bin/sh", script_path, extract_dir, NULL};
        if (run_cmd(sh_argv) != 0) {
            rollback_created_files(created);
            fclose(created);
            remove_tree(reg_dir);
            dief("install script failed");
        }
        if (write_snapshot(roots, nroots, after_path) != 0) {
            rollback_created_files(created);
            fclose(created);
            remove_tree(reg_dir);
            dief("failed to create post-install snapshot");
        }
        char diff_cmd[2048];
        snprintf(diff_cmd, sizeof(diff_cmd), "comm -13 %s %s", before_path, after_path);
        FILE *diff = popen(diff_cmd, "r");
        if (!diff) {
            rollback_created_files(created);
            fclose(created);
            remove_tree(reg_dir);
            dief("failed to diff filesystem snapshots");
        }
        char line[1024];
        while (fgets(line, sizeof(line), diff)) {
            trim_crlf(line);
            if (line[0] == '\0') continue;
            if (!safe_managed_path(line)) {
                log_warn("skipping unsafe path from filesystem diff: %s", line);
                continue;
            }
            fprintf(created, "%s\n", line);
            installed_files++;
        }
        int diff_rc = pclose(diff);
        if (diff_rc != 0 && diff_rc != 1) {
            rollback_created_files(created);
            fclose(created);
            remove_tree(reg_dir);
            dief("filesystem snapshot diff failed");
        }
        if (fflush(created) != 0) {
            rollback_created_files(created);
            fclose(created);
            remove_tree(reg_dir);
            dief("failed to finalize install transaction");
        }
    } else {
        if (install_payload_recursive(payload_path, install_dir, created, 0,
                                      &installed_files) != 0 ||
            fflush(created) != 0) {
            rollback_created_files(created);
            fclose(created);
            remove_tree(reg_dir);
            dief("payload installation failed (file collision or I/O error)");
        }
    }

    char files_path[1024];
    snprintf(files_path, sizeof(files_path), "%s/files", reg_dir);
    int files_fd =
        open(files_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    FILE *flist = files_fd >= 0 ? fdopen(files_fd, "w") : NULL;
    if (!flist) {
        if (files_fd >= 0) close(files_fd);
        rollback_created_files(created);
        fclose(created);
        remove_tree(reg_dir);
        dief("failed to create package file registry");
    }
    rewind(created);
    char registry_buffer[4096];
    int files_ok = 1;
    for (;;) {
        size_t got = fread(registry_buffer, 1, sizeof(registry_buffer), created);
        if (got && fwrite(registry_buffer, 1, got, flist) != got) {
            files_ok = 0;
            break;
        }
        if (got < sizeof(registry_buffer)) {
            if (ferror(created)) files_ok = 0;
            break;
        }
    }
    if (fclose(flist) != 0) files_ok = 0;
    if (!files_ok) {
        rollback_created_files(created);
        fclose(created);
        remove_tree(reg_dir);
        dief("failed to save package file registry");
    }
    fclose(created);

    /* update required_by for each dependency that's already installed */
    for (int i = 0; i < pkg.depends_count; i++) {
        if (is_installed(pkg.depends[i])) {
            manifest_list_add(pkg.depends[i], "required_by", name);
        }
    }

    /* => installed to /path */
    fprintf(stdout, "%s=>%s installed to %s%s%s\n", ANSI_CYAN, ANSI_RESET, ANSI_DIM, install_dir, ANSI_RESET);
    fprintf(stdout, "%s[*]%s %s %sinstalled%s\n", ANSI_GREEN, ANSI_RESET, pkg.name, ANSI_GREEN, ANSI_RESET);

    if (remove_tree(tmp_dir) != 0)
        log_warn("could not clean temporary files");
    else
        g_active_tmp[0] = '\0';
    return 0;
}
#pragma GCC diagnostic pop

void cmd_repo(const char *subcmd, const char *arg) {
    if (!subcmd) {
        dief("usage: pkg repo <show|add|remove|ping>");
    }

    if (strcmp(subcmd, "show") == 0) {
        RepoConfig repos[MAX_REPOS];
        int count = read_repos(repos, MAX_REPOS);
        if (count == 0) {
            log_info("no repositories configured (edit %s)", REPO_SOURCES_PATH);
            return;
        }
        fprintf(stdout, "  %-12s %-8s %s\n", "REPOSITORY", "PRIO", "URL");
        for (int i = 0; i < count; i++) {
            fprintf(stdout, "  %s%-12s%s %s%-8d%s %s\n",
                    ANSI_CYAN, repos[i].name, ANSI_RESET,
                    ANSI_GREEN, repos[i].priority, ANSI_RESET,
                    repos[i].url);
        }
        fprintf(stdout, "\n  %d repository(ies) configured\n", count);
        return;
    }

    if (strcmp(subcmd, "add") == 0) {
        if (!arg) dief("usage: pkg repo add <name> <url> [priority]");

        char name[128] = "", url[512] = "";
        int priority = 50;

        char buf[1024];
        if (strlen(arg) >= sizeof(buf)) dief("repository arguments are too long");
        snprintf(buf, sizeof(buf), "%s", arg);
        char *tok = strtok(buf, " \t");
        if (tok) snprintf(name, sizeof(name), "%s", tok);
        tok = strtok(NULL, " \t");
        if (tok) snprintf(url, sizeof(url), "%s", tok);
        tok = strtok(NULL, " \t");
        if (tok) {
            int val = 0;
            int neg = 1;
            const char *p = tok;
            if (*p == '-') { neg = -1; p++; }
            if (!isdigit((unsigned char) *p)) dief("invalid repository priority");
            while (*p >= '0' && *p <= '9') {
                if (val > (INT_MAX - (*p - '0')) / 10)
                    dief("repository priority is out of range");
                val = val * 10 + (*p - '0');
                p++;
            }
            if (*p) dief("invalid repository priority");
            priority = val * neg;
        }

        if (!valid_repo_name(name) || !valid_repo_url(url))
            dief("invalid repository name or URL");
        add_repo(name, url, priority);
        return;
    }

    if (strcmp(subcmd, "remove") == 0 || strcmp(subcmd, "rm") == 0) {
        if (!arg) dief("usage: pkg repo remove <name>");
        remove_repo(arg);
        return;
    }

    if (strcmp(subcmd, "ping") == 0) {
        RepoConfig repos[MAX_REPOS];
        int count = read_repos(repos, MAX_REPOS);
        if (count == 0) {
            log_info("no repositories configured");
            return;
        }
        for (int i = 0; i < count; i++) {
            char url[1200];
            snprintf(url, sizeof(url), "%s/health", repos[i].url);

            log_step("pinging", "%s (%s)", repos[i].name, repos[i].url);

            int code = 0;
            char *body = http_get_body(url, &code);
            free(body);

            if (code == 200)
                log_done("%s online", repos[i].name);
            else
                log_warn("%s not responding (HTTP %d)", repos[i].name, code);
        }
        return;
    }

    dief("unknown repo subcommand '%s'", subcmd);
}

void cmd_get(const char *name) {
    ResolvedPkg install_list[MAX_DEPS * 2];
    memset(install_list, 0, sizeof(install_list));
    char visited[MAX_DEP_DEPTH][128];
    int vis_count = 0;

    resolve_dependencies(name, install_list, sizeof(install_list) / sizeof(install_list[0]),
                         visited, &vis_count);

    int count = 0;
    for (int i = 0; i < (int)(sizeof(install_list) / sizeof(install_list[0])); i++) {
        if (install_list[i].name[0]) count++;
    }

    if (count == 0) {
        if (is_installed(name))
            log_info("'%s' is already installed", name);
        else
            dief("nothing to install for '%s'", name);
        return;
    }

    long total_download = 0;
    for (int i = 0; i < count; i++) {
        char url[1400];
        snprintf(url, sizeof(url), "%s/packages/%s/archive", install_list[i].endpoint, install_list[i].name);
        long sz = http_content_length(url);
        install_list[i].download_size = sz > 0 ? sz : 0;
        total_download += install_list[i].download_size;
    }

    long estimated_disk = total_download > 0 ? total_download * 5 / 2 : 0;
    long avail = disk_available("/usr");

    fprintf(stdout, "\n  The following packages will be installed:\n  ");
    for (int i = 0; i < count; i++) {
        if (i > 0) fprintf(stdout, " + ");
        long sz = install_list[i].download_size;
        if (sz > 1048576)
            fprintf(stdout, "%s (%s) [%ld.%ld MB]", install_list[i].name, install_list[i].version,
                    sz / 1048576, (sz % 1048576) * 10 / 1048576);
        else if (sz > 0)
            fprintf(stdout, "%s (%s) [%ld KB]", install_list[i].name, install_list[i].version, sz / 1024);
        else
            fprintf(stdout, "%s (%s)", install_list[i].name, install_list[i].version);
    }
    fprintf(stdout, "\n");

    fprintf(stdout, "  Total download size: ");
    if (total_download > 1048576)
        fprintf(stdout, "%ld.%ld MB", total_download / 1048576, (total_download % 1048576) * 10 / 1048576);
    else if (total_download > 0)
        fprintf(stdout, "%ld KB", total_download / 1024);
    else
        fprintf(stdout, "unknown");

    fprintf(stdout, " | Required disk space: ");
    if (estimated_disk > 1048576)
        fprintf(stdout, "%ld.%ld MB", estimated_disk / 1048576, (estimated_disk % 1048576) * 10 / 1048576);
    else if (estimated_disk > 0)
        fprintf(stdout, "%ld KB", estimated_disk / 1024);
    else
        fprintf(stdout, "unknown");
    fprintf(stdout, "\n");

    fprintf(stdout, "  Space available: ");
    if (avail >= 0) {
        if (avail > 1073741824L)
            fprintf(stdout, "%ld.%ld GB", avail / 1073741824L, (avail % 1073741824L) * 10 / 1073741824L);
        else if (avail > 1048576)
            fprintf(stdout, "%ld.%ld MB", avail / 1048576, (avail % 1048576) * 10 / 1048576);
        else
            fprintf(stdout, "%ld KB", avail / 1024);
    } else {
        fprintf(stdout, "unknown");
    }
    fprintf(stdout, "\n");

    if (avail >= 0 && estimated_disk > 0 && avail < estimated_disk) {
        fprintf(stderr, "\n  %s[!]%s Not enough disk space\n", ANSI_YELLOW, ANSI_RESET);
        return;
    }

    if (!yes_mode && !confirm_prompt("\n  Do you want to continue? [Y/n] ")) {
        fprintf(stdout, "\n");
        log_info("aborted");
        return;
    }
    fprintf(stdout, "\n");

    for (int i = 0; i < count; i++) {
        /* top-level package is last in list; deps get top-level name as requester */
        const char *req = (i < count - 1) ? name : NULL;
        int expl = (i == count - 1) ? 1 : 0;
        do_install(install_list[i].name, install_list[i].endpoint, install_list[i].download_size, req, expl);
    }

    fprintf(stdout, "\n  %d package%s installed successfully:\n", count, count == 1 ? "" : "s");
    for (int i = 0; i < count; i++) {
        fprintf(stdout, "    - %s %s\n", install_list[i].name, install_list[i].version);
    }
    fprintf(stdout, "\n");
}

void cmd_list(void) {
    char reg_dir[512];
    snprintf(reg_dir, sizeof(reg_dir), "%s/installed", PKG_STATE_DIR);

    DIR *d = opendir(reg_dir);
    if (!d) {
        log_info("no packages installed");
        return;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!valid_pkg_name(ent->d_name)) continue;

        char manifest_path[1024];
        snprintf(manifest_path, sizeof(manifest_path), "%s/%s/manifest", reg_dir, ent->d_name);

        FILE *f = fopen(manifest_path, "r");
        if (f) {
            char version[64] = "?";
            char desc[256] = "";
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "version=", 8) == 0) {
                    const char *val = line + 8;
                    while (*val == ' ' || *val == '\t') val++;
                    size_t vlen = 0;
                    while (val[vlen] && val[vlen] != '\n' && val[vlen] != '\r') vlen++;
                    if (vlen > 63) vlen = 63;
                    memcpy(version, val, vlen);
                    version[vlen] = '\0';
                    continue;
                }
                if (strncmp(line, "description=", 12) == 0) {
                    const char *val = line + 12;
                    while (*val == ' ' || *val == '\t') val++;
                    size_t vlen = 0;
                    while (val[vlen] && val[vlen] != '\n' && val[vlen] != '\r') vlen++;
                    if (vlen > 255) vlen = 255;
                    memcpy(desc, val, vlen);
                    desc[vlen] = '\0';
                    continue;
                }
            }
            fclose(f);
            fprintf(stdout, "  %s%-20s%s %s%-8s%s %s\n",
                    ANSI_CYAN, ent->d_name, ANSI_RESET,
                    ANSI_GREEN, version, ANSI_RESET,
                    desc);
        } else {
            fprintf(stdout, "  %s%-20s%s\n", ANSI_CYAN, ent->d_name, ANSI_RESET);
        }
        count++;
    }
    closedir(d);

    if (count == 0) {
        log_info("no packages installed");
    } else {
        fprintf(stdout, "\n  %d package(s) installed\n", count);
    }
}

void cmd_remove(const char *name) {
    if (!valid_pkg_name(name)) dief("invalid package name");
    char reg_dir[512];
    snprintf(reg_dir, sizeof(reg_dir), "%s/installed/%s", PKG_STATE_DIR, name);

    struct stat st;
    if (stat(reg_dir, &st) != 0) {
        dief("package '%s' is not installed", name);
    }

    /* check reverse dependencies: refuse if other packages depend on this one */
    char rb_items[MAX_DEPS][128];
    int rb_count = manifest_read_list(name, "required_by", rb_items, MAX_DEPS);
    if (rb_count > 0) {
        fprintf(stderr, "\n  %s[!]%s Cannot remove '%s': the following packages depend on it:\n", ANSI_YELLOW, ANSI_RESET, name);
        for (int i = 0; i < rb_count; i++)
            fprintf(stderr, "    - %s\n", rb_items[i]);
        fprintf(stderr, "\n  Remove dependent packages first.\n\n");
        return;
    }

    /* read this package's forward dependencies */
    char deps[MAX_DEPS][128];
    int dep_count = manifest_read_list(name, "depends", deps, MAX_DEPS);

    log_step("removing", "%s", name);

    int removed = 0;

    char files_path[1024];
    snprintf(files_path, sizeof(files_path), "%s/files", reg_dir);

    FILE *f = fopen(files_path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
            if (len == 0) continue;

            struct stat fst;
            if (!safe_managed_path(line)) {
                log_warn("refusing unsafe registry path: %s", line);
                continue;
            }
            if (lstat(line, &fst) == 0) {
                if ((S_ISREG(fst.st_mode) || S_ISLNK(fst.st_mode)) &&
                    unlink(line) == 0) {
                    removed++;
                } else {
                    log_warn("could not remove %s", line);
                }
            }
        }
        fclose(f);

        /* second pass: remove empty directories */
        f = fopen(files_path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
                if (len == 0) continue;
                rmdir(line);
            }
            fclose(f);
        }
    }

    /* remove this package from each dependency's required_by */
    for (int i = 0; i < dep_count; i++) {
        manifest_list_remove(deps[i], "required_by", name);
    }

    /* delete the package registration directory */
    if (remove_tree(reg_dir) != 0) log_warn("could not remove package registry");

    log_done("removed %s (%d file%s deleted)", name, removed, removed == 1 ? "" : "s");

    /* scan for orphaned dependencies and suggest autoremove */
    char orphans[32][256];
    int orphan_count = 0;

    char installed_dir[512];
    snprintf(installed_dir, sizeof(installed_dir), "%s/installed", PKG_STATE_DIR);
    DIR *d = opendir(installed_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!valid_pkg_name(ent->d_name)) continue;
            if (strcmp(ent->d_name, name) == 0) continue; /* skip the one we just removed */
            char items[MAX_DEPS][128];
            int cnt = manifest_read_list(ent->d_name, "required_by", items, MAX_DEPS);
            if (cnt == 0 && orphan_count < 32) {
                /* check if this package has deps itself (i.e. it's a leaf dependency) */
                char d_items[MAX_DEPS][128];
                int dc = manifest_read_list(ent->d_name, "depends", d_items, MAX_DEPS);
                if (dc > 0) {
                    snprintf(orphans[orphan_count], sizeof(orphans[orphan_count]), "%s", ent->d_name);
                    orphan_count++;
                }
            }
        }
        closedir(d);
    }

    if (orphan_count > 0) {
        fprintf(stdout, "\n  The following package%s no longer required:\n", orphan_count == 1 ? " is" : "s are");
        for (int i = 0; i < orphan_count; i++)
            fprintf(stdout, "    - %s\n", orphans[i]);
        fprintf(stdout, "  Run %spkg autoremove%s to remove them.\n\n", ANSI_BOLD, ANSI_RESET);
    }
}

void cmd_autoremove(void) {
    char installed_dir[512];
    snprintf(installed_dir, sizeof(installed_dir), "%s/installed", PKG_STATE_DIR);

    /* first pass: collect orphan candidates */
    char orphans[64][256];
    int orphan_count = 0;

    DIR *d = opendir(installed_dir);
    if (!d) {
        log_info("no packages installed");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!valid_pkg_name(ent->d_name)) continue;
        char items[MAX_DEPS][128];
        int cnt = manifest_read_list(ent->d_name, "required_by", items, MAX_DEPS);
        /* only auto-installed packages with no dependents are orphan candidates */
        if (cnt == 0 && orphan_count < 64) {
            char expl_val[16] = "";
            manifest_read_field(ent->d_name, "explicit", expl_val, sizeof(expl_val));
            if (strcmp(expl_val, "1") != 0) {
                snprintf(orphans[orphan_count], sizeof(orphans[orphan_count]), "%s", ent->d_name);
                orphan_count++;
            }
        }
    }
    closedir(d);

    if (orphan_count == 0) {
        log_info("no orphaned packages to remove");
        return;
    }

    fprintf(stdout, "\n  The following packages are no longer required:\n");
    for (int i = 0; i < orphan_count; i++)
        fprintf(stdout, "    - %s\n", orphans[i]);
    fprintf(stdout, "\n");

    /* prompt (skip if -y/--yes) */
    if (!yes_mode && !confirm_prompt("  Do you want to remove them? [Y/n] ")) {
        fprintf(stdout, "\n");
        log_info("aborted");
        return;
    }
    fprintf(stdout, "\n");

    /* second pass: remove orphans (iterate until no more orphans found) */
    int total_removed = 0;
    int changed = 1;
    while (changed) {
        changed = 0;
        d = opendir(installed_dir);
        if (!d) break;

        while ((ent = readdir(d)) != NULL) {
            if (!valid_pkg_name(ent->d_name)) continue;

            char items[MAX_DEPS][128];
            int cnt = manifest_read_list(ent->d_name, "required_by", items, MAX_DEPS);
            if (cnt > 0) continue;
            char expl_val[16] = "";
            manifest_read_field(ent->d_name, "explicit", expl_val, sizeof(expl_val));
            if (strcmp(expl_val, "1") == 0) continue;

            /* orphan found: remove it */
            fprintf(stdout, "  %s=>%s removing %s%s%s\n", ANSI_CYAN, ANSI_RESET, ANSI_BOLD, ent->d_name, ANSI_RESET);

            /* read its deps and remove this package from their required_by */
            char deps[MAX_DEPS][128];
            int dep_count = manifest_read_list(ent->d_name, "depends", deps, MAX_DEPS);
            for (int i = 0; i < dep_count; i++) {
                manifest_list_remove(deps[i], "required_by", ent->d_name);
            }

            /* delete files */
            char pkg_dir[1024];
            snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", installed_dir, ent->d_name);

            char files_path[1280];
            snprintf(files_path, sizeof(files_path), "%s/files", pkg_dir);
            FILE *f = fopen(files_path, "r");
            if (f) {
                char line[512];
                while (fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
                    if (len == 0) continue;
                    struct stat fst;
                    if (safe_managed_path(line) && lstat(line, &fst) == 0 &&
                        (S_ISREG(fst.st_mode) || S_ISLNK(fst.st_mode)))
                        unlink(line);
                }
                fclose(f);
            }

            if (remove_tree(pkg_dir) != 0)
                log_warn("could not remove registry for %s", ent->d_name);
            total_removed++;
            changed = 1;
        }
        closedir(d);
    }

    if (total_removed > 0)
        log_done("removed %d orphaned package%s", total_removed, total_removed == 1 ? "" : "s");
    else
        log_info("no orphaned packages to remove");
}

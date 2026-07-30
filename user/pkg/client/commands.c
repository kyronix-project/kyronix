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
        pid_t sp = spinner_start();
        char *manifest = http_get_body(url, &code);
        spinner_stop(sp);
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

static void scan_files_recursive(const char *dir, FILE *out) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            fprintf(out, "%s\n", path);
        } else if (S_ISDIR(st.st_mode)) {
            scan_files_recursive(path, out);
        }
    }
    closedir(d);
}

static int file_list_contains(const char *buf, size_t buf_sz, const char *line, size_t line_sz) {
    if (!buf || !line || line_sz == 0) return 0;
    size_t offset = 0;
    while (offset < buf_sz) {
        const char *hit = memmem(buf + offset, buf_sz - offset, line, line_sz);
        if (!hit) return 0;
        size_t pos = (size_t)(hit - buf);
        if (pos > 0 && buf[pos - 1] != '\n') { offset = pos + 1; continue; }
        size_t end = pos + line_sz;
        if (end < buf_sz && buf[end] != '\n') { offset = end + 1; continue; }
        return 1;
    }
    return 0;
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
    if (!valid_pkg_name(name)) dief("invalid resolved package");
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

    char archive_path[512], checksum_path[512], script_path[512], extract_dir[512];
    snprintf(archive_path, sizeof(archive_path), "%s/%s", tmp_dir, "package.gz");
    snprintf(checksum_path, sizeof(checksum_path), "%s/%s", tmp_dir, "package.gz.md5");
    snprintf(script_path, sizeof(script_path), "%s/%s", tmp_dir, "install.sh");
    snprintf(extract_dir, sizeof(extract_dir), "%s/%s", tmp_dir, "extract");
    ensure_dir(extract_dir);

    const char *install_dir = "/usr/bin";
    char base[2048];
    snprintf(base, sizeof(base), "%s/packages/%s", endpoint, name);

    char url_archive[2200], url_checksum[2200], url_script[2200];
    snprintf(url_archive, sizeof(url_archive), "%s/archive", base);
    snprintf(url_checksum, sizeof(url_checksum), "%s/checksum", base);
    snprintf(url_script, sizeof(url_script), "%s/install.sh", base);

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
    if (http_download(url_script, script_path, "script") != 0) dief("install script download failed");

    fprintf(stdout, "%s=>%s verifying checksum...", ANSI_CYAN, ANSI_RESET);
    fflush(stdout);
    if (verify_checksum(archive_path, checksum_path) != 0) dief("checksum mismatch - archive corrupted");
    fprintf(stdout, " %sok%s\n", ANSI_GREEN, ANSI_RESET);

    fprintf(stdout, "%s=>%s extracting...", ANSI_CYAN, ANSI_RESET);
    fflush(stdout);
    char *tar_argv[] = {
        "tar", "-x", "-I", "gzip -dcf", "-f", archive_path,
        "-C", extract_dir,
        "--transform=flags=r;s|^\\.$|__skip__|",
        "--no-same-owner", "--no-same-permissions",
        NULL
    };
    if (run_cmd(tar_argv) != 0) dief("archive extraction failed");
    fprintf(stdout, " %sok%s\n", ANSI_GREEN, ANSI_RESET);

    char pre_scan[1024];
    snprintf(pre_scan, sizeof(pre_scan), "%s/.pre", tmp_dir);
    FILE *pf = fopen(pre_scan, "w");
    if (pf) { scan_files_recursive("/usr", pf); fclose(pf); }

    char *sh_argv[] = { "sh", script_path, extract_dir, archive_path, checksum_path, (char *)install_dir, NULL };
    if (run_cmd(sh_argv) != 0) dief("install script failed");

    char reg_parent[512];
    snprintf(reg_parent, sizeof(reg_parent), "%s", PKG_STATE_DIR);
    ensure_dir(reg_parent);
    snprintf(reg_parent, sizeof(reg_parent), "%s/installed", PKG_STATE_DIR);
    ensure_dir(reg_parent);

    char reg_dir[1024];
    snprintf(reg_dir, sizeof(reg_dir), "%s/installed/%s", PKG_STATE_DIR, name);
    ensure_dir(reg_dir);

    char files_path[1024];
    snprintf(files_path, sizeof(files_path), "%s/files", reg_dir);
    FILE *flist = fopen(files_path, "w");

    char *pre_names = NULL;
    size_t pre_sz = 0;
    pre_names = read_file(pre_scan, &pre_sz);

    char post_file[1024];
    snprintf(post_file, sizeof(post_file), "%s/.post", tmp_dir);
    FILE *psf = fopen(post_file, "w");
    if (psf) { scan_files_recursive("/usr", psf); fclose(psf); }

    size_t post_sz = 0;
    char *post_names = read_file(post_file, &post_sz);
    if (post_names && flist) {
        char *p = post_names;
        while (*p) {
            char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > 0 && !file_list_contains(pre_names, pre_sz, p, len)) {
                fprintf(flist, "%.*s\n", (int)len, p);
                if (strncmp(p, "/usr/bin/", 9) == 0) {
                    char fpath[1024];
                    snprintf(fpath, sizeof(fpath), "%.*s", (int)len, p);
                    chmod(fpath, 0755);
                }
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
    free(post_names);
    free(pre_names);
    if (flist) fclose(flist);

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest", reg_dir);
    FILE *mf = fopen(manifest_path, "w");
    if (mf) {
        fprintf(mf, "name=%s\n", pkg.name);
        fprintf(mf, "version=%s\n", pkg.version);
        fprintf(mf, "description=%s\n", pkg.description);
        fprintf(mf, "arch=%s\n", pkg.arch);
        fprintf(mf, "install_dir=%s\n", install_dir);
        fprintf(mf, "depends=");
        for (int i = 0; i < pkg.depends_count; i++)
            fprintf(mf, "%s%s", i > 0 ? "," : "", pkg.depends[i]);
        fprintf(mf, "\n");
        fprintf(mf, "required_by=%s\n", requested_by ? requested_by : "");
        fprintf(mf, "explicit=%d\n", explicit);
        fclose(mf);
    }

    /* update required_by for each dependency that's already installed */
    for (int i = 0; i < pkg.depends_count; i++) {
        if (is_installed(pkg.depends[i]))
            manifest_list_add(pkg.depends[i], "required_by", name);
    }

    fprintf(stdout, "%s=>%s installed to %s%s%s\n", ANSI_CYAN, ANSI_RESET, ANSI_DIM, install_dir, ANSI_RESET);
    fprintf(stdout, "%s[*]%s %s %sinstalled%s\n", ANSI_GREEN, ANSI_RESET, pkg.name, ANSI_GREEN, ANSI_RESET);

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
            pid_t sp = spinner_start();
            char *body = http_get_body(url, &code);
            spinner_stop(sp);
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

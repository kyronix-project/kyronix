#define _GNU_SOURCE

#include "commands.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "http.h"
#include "md5.h"
#include "util.h"

int verbose_mode = 0;
int yes_mode = 0;

static int extract_json_string(const char *json, const char *key, char *out, size_t n) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return -1;
    snprintf(out, n, "%.*s", (int)(e - p), p);
    return 0;
}

static int parse_depends(const char *json, PackageInfo *pkg) {
    pkg->depends_count = 0;
    const char *p = strstr(json, "\"depends\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    while (*p && *p != ']' && pkg->depends_count < MAX_DEPS) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r')) p++;
        if (*p == '"' ) {
            p++;
            const char *end = strchr(p, '"');
            if (!end) break;
            size_t len = (size_t)(end - p);
            if (len >= 128) len = 127;
            memcpy(pkg->depends[pkg->depends_count], p, len);
            pkg->depends[pkg->depends_count][len] = '\0';
            pkg->depends_count++;
            p = end + 1;
        } else {
            break;
        }
    }
    return 0;
}

static int parse_manifest(const char *json, PackageInfo *pkg) {
    memset(pkg, 0, sizeof(*pkg));
    if (extract_json_string(json, "name", pkg->name, sizeof(pkg->name)) != 0) return -1;
    if (extract_json_string(json, "version", pkg->version, sizeof(pkg->version)) != 0) return -1;
    extract_json_string(json, "description", pkg->description, sizeof(pkg->description));
    if (extract_json_string(json, "arch", pkg->arch, sizeof(pkg->arch)) != 0) return -1;
    extract_json_string(json, "maintainer", pkg->maintainer, sizeof(pkg->maintainer));
    extract_json_string(json, "license", pkg->license, sizeof(pkg->license));
    extract_json_string(json, "homepage", pkg->homepage, sizeof(pkg->homepage));

    const char *rev = strstr(json, "\"revision\"");
    if (rev) {
        rev += 10;
        while (*rev && (*rev == ' ' || *rev == ':')) rev++;
        int val = 0;
        int neg = 1;
        if (*rev == '-') { neg = -1; rev++; }
        while (*rev >= '0' && *rev <= '9') { val = val * 10 + (*rev - '0'); rev++; }
        pkg->revision = val * neg;
    }

    parse_depends(json, pkg);
    return 0;
}

static int verify_checksum(const char *archive_path, const char *checksum_path) {
    size_t len = 0;
    char *txt = read_file(checksum_path, &len);
    if (!txt) return -1;
    trim_crlf(txt);

    char expected[33];
    if (txt[0] == '{') {
        if (extract_json_string(txt, "checksum", expected, sizeof(expected)) != 0) {
            free(txt);
            return -1;
        }
    } else {
        snprintf(expected, sizeof(expected), "%s", txt);
    }

    char actual[33];
    if (md5_file_hex(archive_path, actual) != 0) {
        free(txt);
        return -1;
    }

    int match = strcasecmp(expected, actual) == 0;
    free(txt);
    return match ? 0 : -1;
}

static int is_installed(const char *name) {
    const char *home = home_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.pkg/installed/%s/manifest", home, name);
    struct stat st;
    return stat(path, &st) == 0;
}

static int get_installed_version(const char *name, char *ver, size_t ver_sz) {
    const char *home = home_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.pkg/installed/%s/manifest", home, name);
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

static void local_manifest_path(char *out, size_t n, const char *name) {
    const char *home = home_dir();
    snprintf(out, n, "%s/.pkg/installed/%s/manifest", home, name);
}

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
        size_t nlen = end ? (size_t)(end - p) : strlen(p);
        while (nlen > 0 && p[0] == ' ') { p++; nlen--; }
        while (nlen > 0 && p[nlen-1] == ' ') nlen--;
        if (nlen >= 128) nlen = 127;
        memcpy(items[count], p, nlen);
        items[count][nlen] = '\0';
        count++;
        p += nlen;
    }
    return count;
}

static void manifest_write_field(const char *name, const char *key, const char *value) {
    char path[1024];
    local_manifest_path(path, sizeof(path), name);

    size_t len = 0;
    char *txt = read_file(path, &len);
    char new_content[8192];
    new_content[0] = '\0';

    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    int found = 0;

    if (txt) {
        char *line = txt;
        while (*line) {
            char *eol = strchr(line, '\n');
            size_t llen = eol ? (size_t)(eol - line) : strlen(line);

            if (strncmp(line, needle, strlen(needle)) == 0) {
                char entry[512];
                snprintf(entry, sizeof(entry), "%s=%s\n", key, value);
                strcat(new_content, entry);
                found = 1;
            } else {
                strncat(new_content, line, llen);
                strcat(new_content, "\n");
            }

            line += llen;
            if (*line == '\n') line++;
        }
        free(txt);
    }

    if (!found) {
        char entry[512];
        snprintf(entry, sizeof(entry), "%s=%s\n", key, value);
        strcat(new_content, entry);
    }

    write_text_file(path, new_content);
}

static void manifest_list_add(const char *name, const char *key, const char *item) {
    char items[MAX_DEPS][128];
    int count = manifest_read_list(name, key, items, MAX_DEPS);

    for (int i = 0; i < count; i++) {
        if (strcmp(items[i], item) == 0) return;
    }

    if (count >= MAX_DEPS) return;

    snprintf(items[count], sizeof(items[count]), "%s", item);
    count++;

    char val[4096] = "";
    for (int i = 0; i < count; i++) {
        if (i > 0) strcat(val, ",");
        strcat(val, items[i]);
    }
    manifest_write_field(name, key, val);
}

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
    for (int i = 0; i < new_count; i++) {
        if (i > 0) strcat(val, ",");
        strcat(val, items[i]);
    }
    manifest_write_field(name, key, val);
}
#pragma GCC diagnostic pop


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
            parse_manifest(manifest, &pkg);
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
    char *tar_argv[] = { "tar", "-xzf", archive_path, NULL };
    if (run_cmd_in(extract_dir, tar_argv) != 0) dief("archive extraction failed");
    fprintf(stdout, " %sok%s\n", ANSI_GREEN, ANSI_RESET);

    char pre_scan[1024];
    snprintf(pre_scan, sizeof(pre_scan), "%s/.pre", tmp_dir);
    FILE *pf = fopen(pre_scan, "w");
    if (pf) { scan_files_recursive("/usr", pf); fclose(pf); }

    char *sh_argv[] = { "sh", script_path, extract_dir, archive_path, checksum_path, (char *)install_dir, NULL };
    if (run_cmd(sh_argv) != 0) dief("install script failed");

    const char *home = home_dir();
    char reg_parent[512];
    snprintf(reg_parent, sizeof(reg_parent), "%s/.pkg", home);
    ensure_dir(reg_parent);
    snprintf(reg_parent, sizeof(reg_parent), "%s/.pkg/installed", home);
    ensure_dir(reg_parent);

    char reg_dir[1024];
    snprintf(reg_dir, sizeof(reg_dir), "%s/.pkg/installed/%s", home, name);
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

    for (int i = 0; i < pkg.depends_count; i++) {
        if (is_installed(pkg.depends[i])) {
            manifest_list_add(pkg.depends[i], "required_by", name);
        }
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
            while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
            priority = val * neg;
        }

        if (!name[0] || !url[0]) dief("usage: pkg repo add <name> <url> [priority]");
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

    fprintf(stdout, "\n  Do you want to continue? [Y/n] ");
    fflush(stdout);

    if (!yes_mode) {
        char answer[16] = "";
        if (fgets(answer, sizeof(answer), stdin)) {
            if (answer[0] != '\n' && answer[0] != 'y' && answer[0] != 'Y') {
                fprintf(stdout, "\n");
                log_info("aborted");
                return;
            }
        }
    }
    fprintf(stdout, "\n");

    for (int i = 0; i < count; i++) {
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
    const char *home = home_dir();
    char reg_dir[512];
    snprintf(reg_dir, sizeof(reg_dir), "%s/.pkg/installed", home);

    DIR *d = opendir(reg_dir);
    if (!d) {
        log_info("no packages installed");
        return;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

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
    const char *home = home_dir();
    char reg_dir[512];
    snprintf(reg_dir, sizeof(reg_dir), "%s/.pkg/installed/%s", home, name);

    struct stat st;
    if (stat(reg_dir, &st) != 0) {
        dief("package '%s' is not installed", name);
    }

    char rb_items[MAX_DEPS][128];
    int rb_count = manifest_read_list(name, "required_by", rb_items, MAX_DEPS);
    if (rb_count > 0) {
        fprintf(stderr, "\n  %s[!]%s Cannot remove '%s': the following packages depend on it:\n", ANSI_YELLOW, ANSI_RESET, name);
        for (int i = 0; i < rb_count; i++)
            fprintf(stderr, "    - %s\n", rb_items[i]);
        fprintf(stderr, "\n  Remove them first, or use %s--force%s to override.\n\n", ANSI_BOLD, ANSI_RESET);
        return;
    }

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
            if (stat(line, &fst) == 0) {
                if (remove(line) == 0) {
                    removed++;
                } else {
                    log_warn("could not remove %s", line);
                }
            }
        }
        fclose(f);
    }

    for (int i = 0; i < dep_count; i++) {
        manifest_list_remove(deps[i], "required_by", name);
    }

    char *rm_argv[] = { "rm", "-rf", reg_dir, NULL };
    run_cmd(rm_argv);

    log_done("removed %s (%d file%s deleted)", name, removed, removed == 1 ? "" : "s");

    char orphans[32][256];
    int orphan_count = 0;

    char installed_dir[512];
    snprintf(installed_dir, sizeof(installed_dir), "%s/.pkg/installed", home);
    DIR *d = opendir(installed_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (strcmp(ent->d_name, name) == 0) continue;
            char items[MAX_DEPS][128];
            int cnt = manifest_read_list(ent->d_name, "required_by", items, MAX_DEPS);
            if (cnt == 0 && orphan_count < 32) {
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
    const char *home = home_dir();
    char installed_dir[512];
    snprintf(installed_dir, sizeof(installed_dir), "%s/.pkg/installed", home);

    char orphans[64][256];
    int orphan_count = 0;

    DIR *d = opendir(installed_dir);
    if (!d) {
        log_info("no packages installed");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char items[MAX_DEPS][128];
        int cnt = manifest_read_list(ent->d_name, "required_by", items, MAX_DEPS);
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

    fprintf(stdout, "  Do you want to remove them? [Y/n] ");
    fflush(stdout);

    if (!yes_mode) {
        char answer[16] = "";
        if (fgets(answer, sizeof(answer), stdin)) {
            if (answer[0] != '\n' && answer[0] != 'y' && answer[0] != 'Y') {
                fprintf(stdout, "\n");
                log_info("aborted");
                return;
            }
        }
    }
    fprintf(stdout, "\n");

    int total_removed = 0;
    int changed = 1;
    while (changed) {
        changed = 0;
        d = opendir(installed_dir);
        if (!d) break;

        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            char items[MAX_DEPS][128];
            int cnt = manifest_read_list(ent->d_name, "required_by", items, MAX_DEPS);
            if (cnt > 0) continue;
            char expl_val[16] = "";
            manifest_read_field(ent->d_name, "explicit", expl_val, sizeof(expl_val));
            if (strcmp(expl_val, "1") == 0) continue;

            fprintf(stdout, "  %s=>%s removing %s%s%s\n", ANSI_CYAN, ANSI_RESET, ANSI_BOLD, ent->d_name, ANSI_RESET);

            char deps[MAX_DEPS][128];
            int dep_count = manifest_read_list(ent->d_name, "depends", deps, MAX_DEPS);
            for (int i = 0; i < dep_count; i++) {
                manifest_list_remove(deps[i], "required_by", ent->d_name);
            }

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
                    remove(line);
                }
                fclose(f);
            }

            char *rm_argv[] = { "rm", "-rf", pkg_dir, NULL };
            run_cmd(rm_argv);
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

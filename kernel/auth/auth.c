#include "auth.h"
#include "../fs/vfs.h"
#include "../fs/vfs_internal.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/heap.h"

#define AUTH_MAX_USERS 64
#define AUTH_MAX_GROUPS 32
#define SHA512_CRYPT_ROUNDS 5000

static auth_passwd_t g_passwd[AUTH_MAX_USERS];
static int g_npasswd;
static auth_shadow_t g_shadow[AUTH_MAX_USERS];
static int g_nshadow;
static auth_group_t g_group[AUTH_MAX_GROUPS];
static int g_ngroup;

static void auth_strlcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static long auth_atol(const char *s) {
    long r = 0;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    return r;
}

static char *auth_strtok_r(char *s, char delim, char **save) {
    if (!s) s = *save;
    if (!s || !*s) return NULL;
    char *start = s;
    while (*s && *s != delim) s++;
    if (*s) {
        *s = 0;
        *save = s + 1;
    } else {
        *save = NULL;
    }
    return start;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    *e = 0;
    return s;
}

static int split(char *s, char delim, char **fields, int max_fields) {
    int n = 0;
    fields[n++] = s;
    for (char *p = s; *p && n < max_fields; p++) {
        if (*p == delim) {
            *p = 0;
            fields[n++] = p + 1;
        }
    }
    return n;
}

static void load_passwd(void) {
    int fd = fd_open_host("/etc/passwd", 0, 0);
    if (fd < 0) return;
    char buf[4096];
    int64_t n = fd_read(fd, buf, sizeof(buf) - 1);
    fd_close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    char *save = NULL;
    for (char *line = auth_strtok_r(buf, '\n', &save); line;
         line = auth_strtok_r(NULL, '\n', &save)) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *f[8];
        if (split(l, ':', f, 7) < 7) continue;
        if (g_npasswd >= AUTH_MAX_USERS) break;
        auth_passwd_t *p = &g_passwd[g_npasswd++];
        memset(p, 0, sizeof(*p));
        auth_strlcpy(p->name, f[0], AUTH_NAME_MAX);
        p->uid = (uint32_t) auth_atol(f[2]);
        p->gid = (uint32_t) auth_atol(f[3]);
        auth_strlcpy(p->gecos, f[4], AUTH_GECOS_MAX);
        auth_strlcpy(p->home, f[5], AUTH_DIR_MAX);
        auth_strlcpy(p->shell, f[6], AUTH_SHELL_MAX);
    }
}

static void load_shadow(void) {
    int fd = fd_open_host("/etc/shadow", 0, 0);
    if (fd < 0) return;
    char buf[4096];
    int64_t n = fd_read(fd, buf, sizeof(buf) - 1);
    fd_close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    char *save = NULL;
    for (char *line = auth_strtok_r(buf, '\n', &save); line;
         line = auth_strtok_r(NULL, '\n', &save)) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *f[10];
        int nf = split(l, ':', f, 9);
        if (nf < 2) continue;
        if (g_nshadow >= AUTH_MAX_USERS) break;
        auth_shadow_t *s = &g_shadow[g_nshadow++];
        memset(s, 0, sizeof(*s));
        auth_strlcpy(s->name, f[0], AUTH_NAME_MAX);
        auth_strlcpy(s->hash, f[1], AUTH_PASS_MAX);
        if (nf > 2) s->lastchg = auth_atol(f[2]);
        if (nf > 3) s->min = auth_atol(f[3]);
        if (nf > 4) s->max = auth_atol(f[4]);
        if (nf > 5) s->warn = auth_atol(f[5]);
        if (nf > 6) s->inactive = auth_atol(f[6]);
        if (nf > 7) s->expire = auth_atol(f[7]);
    }
}

static void load_group(void) {
    int fd = fd_open_host("/etc/group", 0, 0);
    if (fd < 0) return;
    char buf[4096];
    int64_t n = fd_read(fd, buf, sizeof(buf) - 1);
    fd_close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    char *save = NULL;
    for (char *line = auth_strtok_r(buf, '\n', &save); line;
         line = auth_strtok_r(NULL, '\n', &save)) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *f[5];
        if (split(l, ':', f, 4) < 3) continue;
        if (g_ngroup >= AUTH_MAX_GROUPS) break;
        auth_group_t *g = &g_group[g_ngroup++];
        memset(g, 0, sizeof(*g));
        auth_strlcpy(g->name, f[0], AUTH_NAME_MAX);
        g->gid = (uint32_t) auth_atol(f[2]);
        if (f[3]) auth_strlcpy(g->members, f[3], 256);
    }
}

void auth_init(void) {
    g_npasswd = 0;
    g_nshadow = 0;
    g_ngroup = 0;
    load_passwd();
    load_shadow();
    load_group();
    log_info("auth: %d users, %d shadow entries, %d groups", g_npasswd, g_nshadow, g_ngroup);
}

int auth_passwd_count(void) { return g_npasswd; }
const auth_passwd_t *auth_passwd_get(int idx) {
    if (idx < 0 || idx >= g_npasswd) return NULL;
    return &g_passwd[idx];
}
const auth_passwd_t *auth_passwd_byname(const char *name) {
    for (int i = 0; i < g_npasswd; i++)
        if (strcmp(g_passwd[i].name, name) == 0) return &g_passwd[i];
    return NULL;
}
const auth_passwd_t *auth_passwd_byuid(uint32_t uid) {
    for (int i = 0; i < g_npasswd; i++)
        if (g_passwd[i].uid == uid) return &g_passwd[i];
    return NULL;
}
int auth_shadow_count(void) { return g_nshadow; }
const auth_shadow_t *auth_shadow_get(int idx) {
    if (idx < 0 || idx >= g_nshadow) return NULL;
    return &g_shadow[idx];
}
const auth_shadow_t *auth_shadow_byname(const char *name) {
    for (int i = 0; i < g_nshadow; i++)
        if (strcmp(g_shadow[i].name, name) == 0) return &g_shadow[i];
    return NULL;
}
int auth_group_count(void) { return g_ngroup; }
const auth_group_t *auth_group_get(int idx) {
    if (idx < 0 || idx >= g_ngroup) return NULL;
    return &g_group[idx];
}
const auth_group_t *auth_group_byname(const char *name) {
    for (int i = 0; i < g_ngroup; i++)
        if (strcmp(g_group[i].name, name) == 0) return &g_group[i];
    return NULL;
}
const auth_group_t *auth_group_bygid(uint32_t gid) {
    for (int i = 0; i < g_ngroup; i++)
        if (g_group[i].gid == gid) return &g_group[i];
    return NULL;
}

bool auth_user_in_group(const char *user, const char *group) {
    const auth_group_t *g = auth_group_byname(group);
    if (!g) return false;
    const char *m = g->members;
    while (*m) {
        const char *e = m;
        while (*e && *e != ',') e++;
        int len = (int) (e - m);
        if (len == (int) strlen(user) && memcmp(m, user, len) == 0) return true;
        m = *e ? e + 1 : e;
    }
    return false;
}

static const char b64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void to64(char **s, uint32_t v, int n) {
    while (n-- > 0) {
        *(*s)++ = b64[v & 0x3F];
        v >>= 6;
    }
}

void auth_sha512(const uint8_t *data, uint32_t len, uint8_t *out);

void auth_sha512_crypt(const char *password, const char *salt, int rounds, char *out,
                       int out_len) {
    if (rounds < 1000) rounds = 1000;
    if (rounds > 999999999) rounds = 999999999;
    uint32_t plen = (uint32_t) strlen(password);
    uint32_t slen = (uint32_t) strlen(salt);
    if (slen > 16) slen = 16;

    uint8_t alt[64];
    auth_sha512((const uint8_t *) password, plen, alt);
    auth_sha512((const uint8_t *) salt, slen, alt + 32);
    auth_sha512(alt, 64, alt);

    uint8_t ctx[64];
    auth_sha512((const uint8_t *) password, plen, ctx);
    auth_sha512((const uint8_t *) salt, slen, ctx + 32);

    uint8_t p_bytes[64];
    auth_sha512((const uint8_t *) password, plen, p_bytes);
    for (uint32_t i = 0; i < plen; i++) auth_sha512(p_bytes, 64, p_bytes);

    uint8_t s_bytes[64];
    auth_sha512((const uint8_t *) salt, slen, s_bytes);
    for (uint32_t i = 0; i < slen; i++) auth_sha512(s_bytes, 64, s_bytes);

    uint8_t digest[64];
    memcpy(digest, alt, 64);

    for (int r = 0; r < rounds; r++) {
        uint8_t tmp[64];
        if (r & 1)
            auth_sha512(p_bytes, 64, tmp);
        else
            auth_sha512(digest, 64, tmp);
        if (r % 3) {
            uint8_t t2[64];
            auth_sha512(s_bytes, 64, t2);
            auth_sha512(t2, 64, t2);
            memcpy(t2, tmp, 64);
            auth_sha512(t2, 64, tmp);
        }
        if (r % 7) {
            uint8_t t2[64];
            auth_sha512(p_bytes, 64, t2);
            memcpy(t2 + 32, tmp, 32);
            auth_sha512(t2, 64, tmp);
        }
        if (r & 1)
            auth_sha512(tmp, 64, digest);
        else {
            memcpy(digest + 32, tmp, 32);
            auth_sha512(digest, 64, digest);
        }
    }

    char *o = out;
    int remain = out_len - 1;
    if (remain < 86) {
        *out = 0;
        return;
    }
    *o++ = '$';
    *o++ = '6';
    *o++ = '$';
    remain -= 3;
    if (rounds != 5000) {
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf), "rounds=%d$", rounds);
        int rl = (int) strlen(rbuf);
        if (rl > remain) {
            *out = 0;
            return;
        }
        memcpy(o, rbuf, rl);
        o += rl;
        remain -= rl;
    }
    if (slen > (uint32_t) remain) slen = (uint32_t) remain;
    memcpy(o, salt, slen);
    o += slen;
    remain -= (int) slen;
    *o++ = '$';
    remain--;
    if (remain < 86) {
        *out = 0;
        return;
    }

    to64(&o, (digest[0] << 16) | (digest[21] << 8) | digest[42], 4);
    to64(&o, (digest[22] << 16) | (digest[43] << 8) | digest[1], 4);
    to64(&o, (digest[44] << 16) | (digest[2] << 8) | digest[23], 4);
    to64(&o, (digest[3] << 16) | (digest[24] << 8) | digest[45], 4);
    to64(&o, (digest[25] << 16) | (digest[46] << 8) | digest[4], 4);
    to64(&o, (digest[47] << 16) | (digest[5] << 8) | digest[26], 4);
    to64(&o, (digest[6] << 16) | (digest[27] << 8) | digest[48], 4);
    to64(&o, (digest[28] << 16) | (digest[49] << 8) | digest[7], 4);
    to64(&o, (digest[29] << 16) | (digest[8] << 8) | digest[50], 4);
    to64(&o, (digest[9] << 16) | (digest[30] << 8) | digest[51], 4);
    to64(&o, (digest[31] << 16) | (digest[10] << 8) | digest[52], 4);
    to64(&o, (digest[11] << 16) | (digest[32] << 8) | digest[53], 4);
    to64(&o, (digest[33] << 16) | (digest[12] << 8) | digest[54], 4);
    to64(&o, (digest[13] << 16) | (digest[34] << 8) | digest[55], 4);
    to64(&o, (digest[35] << 16) | (digest[14] << 8) | digest[56], 4);
    to64(&o, (digest[15] << 16) | (digest[36] << 8) | digest[57], 4);
    to64(&o, (digest[37] << 16) | (digest[16] << 8) | digest[58], 4);
    to64(&o, (digest[17] << 16) | (digest[38] << 8) | digest[59], 4);
    to64(&o, (digest[39] << 16) | (digest[18] << 8) | digest[60], 4);
    to64(&o, (digest[19] << 16) | (digest[40] << 8) | digest[61], 4);
    to64(&o, (digest[41] << 16) | (digest[20] << 8) | digest[62], 4);
    to64(&o, digest[63], 2);
    *o = 0;
}

bool auth_verify_password(const char *user, const char *password) {
    const auth_shadow_t *s = auth_shadow_byname(user);
    if (!s || !s->hash[0]) return false;
    if (s->hash[0] == '!' || s->hash[0] == '*') return false;

    char salt[64];
    const char *h = s->hash;
    if (h[0] == '$' && h[1] == '6' && h[2] == '$') {
        const char *p = h + 3;
        const char *d = strchr(p, '$');
        if (!d) return false;
        int sl = (int) (d - p);
        if (sl > 16) sl = 16;
        memcpy(salt, p, sl);
        salt[sl] = 0;
        char computed[128];
        auth_sha512_crypt(password, salt, 5000, computed, sizeof(computed));
        return strcmp(computed, h) == 0;
    }
    if (h[0] == '$' && h[1] == '1' && h[2] == '$') {
        return false;
    }
    return false;
}

void auth_set_password(const char *user, const char *password) {
    auth_shadow_t *s = (auth_shadow_t *) auth_shadow_byname(user);
    if (!s) return;
    char salt[17];
    for (int i = 0; i < 16; i++) salt[i] = b64[(uint8_t) (i * 7 + 13) & 0x3F];
    salt[16] = 0;
    auth_sha512_crypt(password, salt, 5000, s->hash, AUTH_PASS_MAX);
}

int auth_add_user(const char *name, uint32_t uid, uint32_t gid, const char *home,
                  const char *shell) {
    if (g_npasswd >= AUTH_MAX_USERS) return -1;
    auth_passwd_t *p = &g_passwd[g_npasswd++];
    memset(p, 0, sizeof(*p));
    auth_strlcpy(p->name, name, AUTH_NAME_MAX);
    p->uid = uid;
    p->gid = gid;
    auth_strlcpy(p->home, home, AUTH_DIR_MAX);
    auth_strlcpy(p->shell, shell, AUTH_SHELL_MAX);

    if (g_nshadow < AUTH_MAX_USERS) {
        auth_shadow_t *s = &g_shadow[g_nshadow++];
        memset(s, 0, sizeof(*s));
        auth_strlcpy(s->name, name, AUTH_NAME_MAX);
        auth_strlcpy(s->hash, "!", AUTH_PASS_MAX);
        s->lastchg = 20000;
        s->min = 0;
        s->max = 99999;
        s->warn = 7;
    }
    return 0;
}

int auth_del_user(const char *name) {
    for (int i = 0; i < g_npasswd; i++) {
        if (strcmp(g_passwd[i].name, name) == 0) {
            memmove(&g_passwd[i], &g_passwd[i + 1], (g_npasswd - i - 1) * sizeof(auth_passwd_t));
            g_npasswd--;
            return 0;
        }
    }
    return -1;
}

bool auth_sudo_allowed(const char *user, const char *cmd) {
    (void) cmd;
    if (strcmp(user, "root") == 0) return true;
    return auth_user_in_group(user, "sudo") || auth_user_in_group(user, "wheel");
}

void auth_dump(void) {
    for (int i = 0; i < g_npasswd; i++) {
        log_info("auth: %s:%d:%d:%s", g_passwd[i].name, g_passwd[i].uid, g_passwd[i].gid,
                 g_passwd[i].home);
    }
}

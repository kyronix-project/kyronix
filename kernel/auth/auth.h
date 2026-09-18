#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUTH_NAME_MAX 32
#define AUTH_PASS_MAX 128
#define AUTH_GECOS_MAX 64
#define AUTH_DIR_MAX 64
#define AUTH_SHELL_MAX 64
#define AUTH_GROUPS_MAX 16

typedef struct {
    char name[AUTH_NAME_MAX];
    uint32_t uid;
    uint32_t gid;
    char gecos[AUTH_GECOS_MAX];
    char home[AUTH_DIR_MAX];
    char shell[AUTH_SHELL_MAX];
} auth_passwd_t;

typedef struct {
    char name[AUTH_NAME_MAX];
    char hash[AUTH_PASS_MAX];
    long lastchg;
    long min;
    long max;
    long warn;
    long inactive;
    long expire;
} auth_shadow_t;

typedef struct {
    char name[AUTH_NAME_MAX];
    uint32_t gid;
    char members[256];
} auth_group_t;

void auth_init(void);
int auth_passwd_count(void);
const auth_passwd_t *auth_passwd_get(int idx);
const auth_passwd_t *auth_passwd_byname(const char *name);
const auth_passwd_t *auth_passwd_byuid(uint32_t uid);
int auth_shadow_count(void);
const auth_shadow_t *auth_shadow_get(int idx);
const auth_shadow_t *auth_shadow_byname(const char *name);
int auth_group_count(void);
const auth_group_t *auth_group_get(int idx);
const auth_group_t *auth_group_byname(const char *name);
const auth_group_t *auth_group_bygid(uint32_t gid);
bool auth_user_in_group(const char *user, const char *group);

void auth_sha512_crypt(const char *password, const char *salt, int rounds, char *out,
                       int out_len);
bool auth_verify_password(const char *user, const char *password);
void auth_set_password(const char *user, const char *password);
int auth_add_user(const char *name, uint32_t uid, uint32_t gid, const char *home,
                  const char *shell);
int auth_del_user(const char *name);
bool auth_sudo_allowed(const char *user, const char *cmd);
void auth_dump(void);

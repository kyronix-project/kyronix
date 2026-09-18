#ifndef PKG_COMMANDS_H
#define PKG_COMMANDS_H

void cmd_health(void);
void cmd_repo(const char *subcmd, const char *arg);
void cmd_get(const char *name);
void cmd_list(void);
void cmd_remove(const char *name);
void cmd_autoremove(void);

#endif

#ifndef PKG_UTIL_H
#define PKG_UTIL_H

#include <stddef.h>

#include "pkg.h"

void log_info(const char *fmt, ...);
void log_step(const char *step, const char *fmt, ...);
void log_done(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void dief(const char *fmt, ...);

char *read_file(const char *path, size_t *len_out);
int write_text_file(const char *path, const char *text);
int starts_with(const char *s, const char *p);
void trim_crlf(char *s);
void ensure_dir(const char *path);
void ensure_dir_mode(const char *path, unsigned mode);
int valid_pkg_name(const char *name);
int valid_repo_name(const char *name);
int valid_repo_url(const char *url);
int safe_managed_path(const char *path);
int remove_tree(const char *path);
int confirm_prompt(const char *prompt);
int run_cmd(char *const argv[]);
int run_cmd_in(const char *dir, char *const argv[]);
int run_cmd_input_file(const char *input_path, char *const argv[]);
int run_pipeline_file_in(const char *dir, const char *input_path,
                         char *const producer[], char *const consumer[],
                         size_t max_output);

int read_repos(RepoConfig *repos, int max);
void write_repos(const RepoConfig *repos, int count);
void add_repo(const char *name, const char *url, int priority);
void remove_repo(const char *name);

long disk_available(const char *path);

#endif

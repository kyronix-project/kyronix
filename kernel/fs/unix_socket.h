#pragma once
#include "fs/vfs.h"

int fd_socket(int domain, int type, int proto);
int fd_bind_unix(int fd, const char *path);
int fd_listen_unix(int fd, int backlog);
int fd_accept_unix(int fd, char *path_out, int path_max, int flags);
int fd_connect_unix(int fd, const char *path);
void unix_socket_close(vfs_file_t *f);

bool unix_socket_has_pending(vfs_node_t *n);
void *unix_socket_wait_object(vfs_node_t *n);

#pragma once
#include "fs/vfs.h"

int vfs_fd_alloc_from(int start);
vfs_file_t *vfs_file_alloc(void);
vfs_file_t *vfs_fd_get(int fd);
void vfs_fd_install(int fd, vfs_file_t *f);
void vfs_fd_clear(int fd);
void vfs_file_close(vfs_file_t *f);
void vfs_file_addref(vfs_file_t *f);
vfs_file_t *vfs_file_clone(vfs_file_t *f);
int vfs_fd_adopt(vfs_file_t *f);
void vfs_pipe_drop_write(pipe_t *p);
void vfs_pipe_maybe_free(pipe_t *p);
vfs_node_t *vfs_node_alloc_internal(const char *name, uint8_t type, uint32_t mode);
void vfs_dir_insert_internal(vfs_node_t *dir, vfs_node_t *child);
vfs_node_t *vfs_dir_find_internal(vfs_node_t *dir, const char *name);
bool vfs_may_create_in_internal(vfs_node_t *dir);
void vfs_node_ref_internal(vfs_node_t *n);
void vfs_node_unref_internal(vfs_node_t *n);
void vfs_node_mark_deleted_internal(vfs_node_t *n);

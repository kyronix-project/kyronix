#pragma once
#include <stdbool.h>
#include <stdint.h>

#define IN_ACCESS 0x00000001U
#define IN_MODIFY 0x00000002U
#define IN_ATTRIB 0x00000004U
#define IN_CLOSE_WRITE 0x00000008U
#define IN_CLOSE_NOWRITE 0x00000010U
#define IN_OPEN 0x00000020U
#define IN_MOVED_FROM 0x00000040U
#define IN_MOVED_TO 0x00000080U
#define IN_CREATE 0x00000100U
#define IN_DELETE 0x00000200U
#define IN_DELETE_SELF 0x00000400U
#define IN_MOVE_SELF 0x00000800U
#define IN_ALL_EVENTS 0x00000fffU
#define IN_UNMOUNT 0x00002000U
#define IN_Q_OVERFLOW 0x00004000U
#define IN_IGNORED 0x00008000U
#define IN_ONLYDIR 0x01000000U
#define IN_DONT_FOLLOW 0x02000000U
#define IN_EXCL_UNLINK 0x04000000U
#define IN_MASK_CREATE 0x10000000U
#define IN_MASK_ADD 0x20000000U
#define IN_ISDIR 0x40000000U
#define IN_ONESHOT 0x80000000U


struct vfs_node;

int fd_inotify_init(int flags);
int fd_inotify_add_watch(int fd, const char *path, uint32_t mask);
int fd_inotify_rm_watch(int fd, int wd);
int64_t inotify_read(void *state, char *buf, uint64_t len, int flags);
bool inotify_pollin(void *state);
void inotify_state_addref(void *state);
void inotify_state_unref(void *state);

uint32_t inotify_next_cookie(void);
void inotify_notify_node(struct vfs_node *n, uint32_t mask, uint32_t cookie);
void inotify_notify_parent(struct vfs_node *parent, const char *name, bool is_dir, uint32_t mask,
                           uint32_t cookie);
void inotify_notify_move(struct vfs_node *n, struct vfs_node *old_parent, const char *old_name,
                         struct vfs_node *new_parent, const char *new_name, uint32_t cookie);
void inotify_notify_delete_self(struct vfs_node *n);

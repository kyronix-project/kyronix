#include "fdpipe.h"
#include "fs/pipe.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"

#define EMFILE 24
#define ENOMEM 12

int fd_pipe(int pipefd[2])
{
    pipe_t* p = pipe_alloc();
    if (!p)
        return -(int) ENOMEM;
    p->read_refs = 1;
    p->write_refs = 1;

    int rfd = vfs_fd_alloc_from(0);
    if (rfd < 0) {
        pipe_free(p);
        return -(int) EMFILE;
    }

    vfs_file_t* rf = vfs_file_alloc();
    if (!rf) {
        pipe_free(p);
        return -(int) ENOMEM;
    }
    rf->pipe = p;
    rf->pipe_end = PIPE_END_READ;
    rf->flags = O_RDONLY;
    vfs_fd_install(rfd, rf);

    int wfd = vfs_fd_alloc_from(0);
    if (wfd < 0) {
        vfs_file_close(rf);
        vfs_fd_clear(rfd);
        pipe_free(p);
        return -(int) EMFILE;
    }

    vfs_file_t* wf = vfs_file_alloc();
    if (!wf) {
        vfs_file_close(rf);
        vfs_fd_clear(rfd);
        pipe_free(p);
        return -(int) ENOMEM;
    }
    wf->pipe = p;
    wf->pipe_end = PIPE_END_WRITE;
    wf->flags = O_WRONLY;
    vfs_fd_install(wfd, wf);

    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

int fd_socketpair(int sv[2])
{
    pipe_t* pa = pipe_alloc();
    pipe_t* pb = pipe_alloc();
    if (!pa || !pb) {
        pipe_free(pa);
        pipe_free(pb);
        return -(int) ENOMEM;
    }

    pa->read_refs = 1;
    pa->write_refs = 1;
    pb->read_refs = 1;
    pb->write_refs = 1;

    int fd0 = vfs_fd_alloc_from(0);
    if (fd0 < 0) {
        pipe_free(pa);
        pipe_free(pb);
        return -(int) EMFILE;
    }

    vfs_file_t* f0 = vfs_file_alloc();
    vfs_file_t* f1 = vfs_file_alloc();
    if (!f0 || !f1) {
        if (f0)
            vfs_file_close(f0);
        if (f1)
            vfs_file_close(f1);
        pipe_free(pa);
        pipe_free(pb);
        return -(int) ENOMEM;
    }

    f0->pipe = pa;
    f0->wpipe = pb;
    f0->pipe_end = PIPE_END_READ;
    f0->flags = O_RDWR;
    f1->pipe = pb;
    f1->wpipe = pa;
    f1->pipe_end = PIPE_END_READ;
    f1->flags = O_RDWR;

    vfs_fd_install(fd0, f0);
    int fd1 = vfs_fd_alloc_from(0);
    if (fd1 < 0) {
        vfs_fd_clear(fd0);
        vfs_file_close(f0);
        vfs_file_close(f1);
        return -(int) EMFILE;
    }
    vfs_fd_install(fd1, f1);
    sv[0] = fd0;
    sv[1] = fd1;
    return 0;
}

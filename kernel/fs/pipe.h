#pragma once
#include <stddef.h>
#include <stdint.h>

#define PIPE_BUFSZ 65536
#define PIPE_ANC_SLOTS 8
#define PIPE_ANC_MAXFDS 4

typedef struct {
    void* files[PIPE_ANC_MAXFDS];
    int32_t nfds;
    int32_t _pad;
} pipe_anc_t;

typedef struct {
    uint64_t magic;
    uint8_t buf[PIPE_BUFSZ];
    uint32_t rpos;
    uint32_t count;
    uint32_t write_refs;
    uint32_t read_refs;
    void* waiting_reader;
    void* waiting_writer;
    pipe_anc_t anc_q[PIPE_ANC_SLOTS];
    uint32_t anc_wr;
    uint32_t anc_rd;
} pipe_t;

#define PIPE_END_READ 0
#define PIPE_END_WRITE 1

pipe_t* pipe_alloc(void);
void pipe_free(pipe_t* p);
int64_t pipe_read(pipe_t* p, void* buf, uint64_t len);
int64_t pipe_peek(pipe_t* p, void* buf, uint64_t len, uint64_t skip);
int64_t pipe_write(pipe_t* p, const void* buf, uint64_t len);
int pipe_anc_send(pipe_t* p, void** files, int nfds);
int pipe_anc_recv(pipe_t* p, void** out, int max);

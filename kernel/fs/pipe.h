#pragma once
#include <stddef.h>
#include <stdint.h>

#define PIPE_BUFSZ 4096

typedef struct
{
    uint8_t buf[PIPE_BUFSZ];
    uint32_t rpos;
    uint32_t count;
    uint32_t write_refs;
    uint32_t read_refs;
    void* waiting_reader;
    void* waiting_writer;
} pipe_t;

#define PIPE_END_READ 0
#define PIPE_END_WRITE 1

pipe_t* pipe_alloc(void);
void pipe_free(pipe_t* p);
int64_t pipe_read(pipe_t* p, void* buf, uint64_t len);
int64_t pipe_write(pipe_t* p, const void* buf, uint64_t len);

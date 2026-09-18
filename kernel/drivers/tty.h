#pragma once
#include <stdbool.h>
#include <stdint.h>

/* c_cc indices */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSWTC 7
#define VSTART 8
#define VSTOP 9
#define VSUSP 10

/* c_iflag bits */
#define IGNBRK 0000001
#define BRKINT 0000002
#define IGNPAR 0000004
#define PARMRK 0000010
#define INPCK 0000020
#define ISTRIP 0000040
#define INLCR 0000100
#define IGNCR 0000200
#define ICRNL 0000400
#define IXON 0002000
#define IXOFF 0001000
#define IXANY 0004000

/* c_oflag bits */
#define OPOST 0000001
#define ONLCR 0000004
#define OCRNL 0000010
#define ONOCR 0000020
#define ONLRET 0000040

/* c_cflag bits */
#define CSIZE 0000060
#define CS5 0000000
#define CS6 0000020
#define CS7 0000040
#define CS8 0000060
#define CSTOPB 0000100
#define CREAD 0000200
#define PARENB 0000400
#define PARODD 0001000
#define HUPCL 0002000
#define CLOCAL 0004000

/* c_lflag bits */
#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define IEXTEN 0001000

#define NCCS 19

struct termios_s {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_cc[NCCS];
};

int64_t tty_read(char *buf, uint64_t len);
int64_t tty_write(const char *buf, uint64_t len);
bool tty_data_ready(void);
void tty_putchar(char c);
int tty_get_fg_pgid(void);
void tty_set_fg_pgid(int pgid);
uint32_t tty_get_lflag(void);
void tty_set_lflag(uint32_t lflag);
void tty_get_termios(struct termios_s *t);
void tty_set_termios(const struct termios_s *t);
void tty_process_input(void);
void tty_check_signals(void);

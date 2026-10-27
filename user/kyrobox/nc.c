#include "common.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#define NC_BUF 8192

static void usage(void) {
    fprintf(stderr, "Usage: nc [-l] [-u] [-p port] [host] port\n");
    exit(1);
}

static int writeall(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t) w;
    }
    return 0;
}

static void relay(int sock) {
    struct pollfd pfds[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = sock, .events = POLLIN },
    };
    char buf[NC_BUF];

    for (;;) {
        if (poll(pfds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (pfds[0].fd >= 0 && (pfds[0].revents & POLLIN)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0)
                pfds[0].fd = -1;
            else if (writeall(sock, buf, (size_t) n) < 0)
                return;
        }
        if (pfds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n <= 0) return;
            if (writeall(STDOUT_FILENO, buf, (size_t) n) < 0) return;
        }
    }
}

int main(int argc, char **argv) {
    kx_prog = "nc";
    int listen_mode = 0, udp = 0;
    const char *src_port = NULL;

    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') break;
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'l':
                listen_mode = 1;
                break;
            case 'u':
                udp = 1;
                break;
            case 'p':
                if (i + 1 >= argc) usage();
                src_port = argv[++i];
                break;
            default:
                usage();
            }
        }
    }

    const char *host = NULL, *port = NULL;
    int rem = argc - i;
    if (listen_mode) {
        if (rem >= 1)
            port = argv[i];
        else if (src_port)
            port = src_port;
        else
            usage();
    } else {
        if (rem < 2) usage();
        host = argv[i];
        port = argv[i + 1];
    }

    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = listen_mode ? AI_PASSIVE : 0;

    int err = getaddrinfo(host, port, &hints, &ai);
    if (err) {
        fprintf(stderr, "nc: %s: %s\n", host ? host : "*", gai_strerror(err));
        return 1;
    }

    if (listen_mode) {
        int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            perror("nc: socket");
            freeaddrinfo(ai);
            return 1;
        }
        int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
            perror("nc: bind");
            freeaddrinfo(ai);
            return 1;
        }
        freeaddrinfo(ai);

        if (!udp) {
            if (listen(sock, 1) < 0) {
                perror("nc: listen");
                return 1;
            }
            struct sockaddr_in cli;
            socklen_t clen = sizeof(cli);
            int conn = accept(sock, (struct sockaddr *) &cli, &clen);
            if (conn < 0) {
                perror("nc: accept");
                return 1;
            }
            close(sock);
            relay(conn);
            close(conn);
        } else {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            char b[NC_BUF];
            ssize_t n = recvfrom(sock, b, sizeof(b), 0, (struct sockaddr *) &peer, &plen);
            if (n < 0) {
                perror("nc: recvfrom");
                return 1;
            }
            writeall(STDOUT_FILENO, b, (size_t) n);
            if (connect(sock, (struct sockaddr *) &peer, plen) < 0) {
                perror("nc: connect");
                return 1;
            }
            relay(sock);
            close(sock);
        }
        return 0;
    }

    int sock = -1, ok = 0;
    for (struct addrinfo *rp = ai; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            ok = 1;
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(ai);
    if (!ok) {
        fprintf(stderr, "nc: connect to %s:%s failed\n", host, port);
        return 1;
    }

    relay(sock);
    close(sock);
    return 0;
}

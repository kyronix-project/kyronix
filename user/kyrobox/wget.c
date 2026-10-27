#define _GNU_SOURCE
#include "common.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define WGET_BUF 8192

static void usage(void) {
    fprintf(stderr, "Usage: wget [-O file] [-q] <url>\n");
    exit(1);
}

static void send_req(int fd, const char *host, const char *path) {
    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host);
    write(fd, buf, n);
}

static const char *basename_from_url(const char *url) {
    const char *p = strrchr(url, '/');
    if (p && p[1]) return p + 1;
    return "index.html";
}

int main(int argc, char **argv) {
    kx_prog = "wget";
    const char *outfile = NULL;
    const char *url = NULL;
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc)
            outfile = argv[++i];
        else if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (argv[i][0] != '-')
            url = argv[i];
        else
            usage();
    }
    if (!url) usage();

    const char *host = url;
    const char *path = "/";

    if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
        path = strchr(host, '/');
        if (path) {
            size_t hlen = (size_t) (path - host);
            char *h = malloc(hlen + 1);
            if (!h) {
                perror("wget");
                return 1;
            }
            memcpy(h, host, hlen);
            h[hlen] = '\0';
            host = h;
        } else {
            path = "/";
        }
    } else {
        /* maybe bare host or IP -- try as http:// */
        size_t len = strlen(url) + 20;
        char *u = malloc(len);
        if (!u) {
            perror("wget");
            return 1;
        }
        snprintf(u, len, "http://%s", url);
        url = u;
        host = url + 7;
        path = "/";
    }

    if (!outfile) outfile = basename_from_url(path);

    /* resolve */
    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai_err = getaddrinfo(host, "80", &hints, &ai);
    if (gai_err) {
        fprintf(stderr, "wget: %s: %s\n", host, gai_strerror(gai_err));
        return 1;
    }

    if (!quiet) fprintf(stderr, "--%.*s resolving...\n", 50, url);

    int fd = -1;
    for (struct addrinfo *rp = ai; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) {
        fprintf(stderr, "wget: %s: connect failed\n", host);
        return 1;
    }

    if (!quiet) fprintf(stderr, "connecting to %s:80... connected.\n", host);

    send_req(fd, host, path);

    char buf[WGET_BUF];
    ssize_t n;
    int status = 0;
    int in_headers = 1;
    int out_fd = -1;

    /* if outfile is stdout */
    if (!strcmp(outfile, "-"))
        out_fd = STDOUT_FILENO;
    else
        out_fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (out_fd < 0) {
        kx_warn(outfile);
        close(fd);
        return 1;
    }

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        char *p = buf;

        if (in_headers) {
            /* use the first bytes to scan for status line and end of headers */
            char *end = memmem(buf, (size_t) n, "\r\n\r\n", 4);
            if (!end) end = memmem(buf, (size_t) n, "\n\n", 2);
            if (end) {
                size_t hdr_len = (size_t) (end - buf);
                /* try to parse status */
                if (n >= 12 && memcmp(buf, "HTTP/", 5) == 0) {
                    status = atoi(buf + 9);
                    if (!quiet) fprintf(stderr, "HTTP status %d\n", status);
                }
                /* body starts after headers */
                size_t skip = hdr_len + ((end[1] == '\n' && end > buf && end[-1] != '\r') ? 2 : 4);
                if (end[1] == '\n' && end > buf && end[-1] != '\r') skip = hdr_len + 2;
                p = buf + skip;
                n -= (ssize_t) (p - buf);
                in_headers = 0;
            }
        }

        if (!in_headers && n > 0) {
            if (write(out_fd, p, (size_t) n) < 0) {
                kx_warn(outfile);
                break;
            }
        }
    }

    close(fd);
    if (out_fd != STDOUT_FILENO) close(out_fd);

    if (!quiet) {
        if (status >= 200 && status < 300)
            fprintf(stderr, "done: %s saved.\n", outfile);
        else
            fprintf(stderr, "done (status %d).\n", status);
    }

    if (url != argv[0] && strncmp(url, "http://", 7) == 0) free((void *) (url - 7));
    if (host != url + 7 && host != argv[0]) free((void *) host);

    return (status >= 200 && status < 300) ? 0 : 1;
}

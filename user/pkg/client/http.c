#define _GNU_SOURCE

#include "http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "util.h"

#define HTTP_MAX_HEADER (64U * 1024U)

static int parse_http_url(const char *url, char *host, size_t host_n, int *port,
                          char *path, size_t path_n) {
    if (!url || !starts_with(url, "http://")) return -1;
    const char *authority = url + 7;
    const char *slash = strchr(authority, '/');
    const char *authority_end = slash ? slash : authority + strlen(authority);
    if (authority == authority_end || memchr(authority, '@', (size_t) (authority_end - authority)))
        return -1;

    const char *colon = memchr(authority, ':', (size_t) (authority_end - authority));
    const char *host_end = colon ? colon : authority_end;
    size_t host_len = (size_t) (host_end - authority);
    if (!host_len || host_len >= host_n) return -1;
    for (size_t i = 0; i < host_len; i++) {
        unsigned char c = (unsigned char) authority[i];
        if (!(isalnum(c) || c == '.' || c == '-')) return -1;
    }
    memcpy(host, authority, host_len);
    host[host_len] = '\0';

    *port = 80;
    if (colon) {
        const char *p = colon + 1;
        if (p == authority_end) return -1;
        unsigned value = 0;
        while (p < authority_end) {
            if (*p < '0' || *p > '9') return -1;
            value = value * 10U + (unsigned) (*p - '0');
            if (value > 65535U) return -1;
            p++;
        }
        if (!value) return -1;
        *port = (int) value;
    }

    const char *url_path = slash ? slash : "/";
    size_t path_len = strlen(url_path);
    if (!path_len || path_len >= path_n) return -1;
    for (size_t i = 0; i < path_len; i++) {
        unsigned char c = (unsigned char) url_path[i];
        if (c <= 0x20 || c == 0x7f || c == '#') return -1;
    }
    memcpy(path, url_path, path_len + 1);
    return 0;
}

static int connect_tcp(const char *host, int port) {
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0) {
        log_info("DNS resolution failed for %s:%d", host, port);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
        (void) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void) setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        int old_flags = fcntl(fd, F_GETFL, 0);
        if (old_flags < 0 || fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int connected = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (connected != 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int ready;
            do {
                ready = poll(&pfd, 1, 10000);
            } while (ready < 0 && errno == EINTR);
            if (ready > 0) {
                int socket_error = 0;
                socklen_t error_len = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                               &error_len) == 0 &&
                    socket_error == 0)
                    connected = 0;
            }
        }
        (void) fcntl(fd, F_SETFL, old_flags);
        if (connected == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static ssize_t read_with_timeout(int fd, void *buf, size_t len) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready;
    do {
        ready = poll(&pfd, 1, 30000);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
        errno = ready == 0 ? ETIMEDOUT : errno;
        return -1;
    }
    return read(fd, buf, len);
}

static int write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *) data;
    while (len) {
        ssize_t written = write(fd, p, len);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        p += written;
        len -= (size_t) written;
    }
    return 0;
}

static int make_request(char *out, size_t size, const char *path, const char *host) {
    int n = snprintf(out, size,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept-Encoding: identity\r\n"
                     "Connection: close\r\n\r\n",
                     path, host, USER_AGENT);
    return n >= 0 && (size_t) n < size ? n : -1;
}

unsigned char *http_get_body_raw(const char *url, int *status_code, size_t *body_len) {
    if (status_code) *status_code = 0;
    if (body_len) *body_len = 0;
    char host[256], path[1024];
    int port = 0;
    if (parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        log_warn("invalid repository URL");
        return NULL;
    }

    int fd = connect_tcp(host, port);
    if (fd < 0) return NULL;

    char request[2048];
    int request_len = make_request(request, sizeof(request), path, host);
    if (request_len < 0) {
        close(fd);
        return NULL;
    }

    if (verbose_mode) log_info("GET %s", url);
    if (write_all(fd, request, (size_t) request_len) != 0) {
        log_info("write failed for %s", url);
        close(fd);
        return NULL;
    }

    size_t cap = 65536;
    size_t len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap + 1);
    if (!buf) {
        close(fd);
        dief("out of memory");
    }

    for (;;) {
        if (len == cap) {
            if (cap >= PKG_MAX_METADATA + HTTP_MAX_HEADER) {
                free(buf);
                close(fd);
                log_warn("metadata response exceeds size limit");
                return NULL;
            }
            cap *= 2;
            if (cap > PKG_MAX_METADATA + HTTP_MAX_HEADER)
                cap = PKG_MAX_METADATA + HTTP_MAX_HEADER;
            unsigned char *nb = (unsigned char *)realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                close(fd);
                dief("out of memory");
            }
            buf = nb;
        }
        ssize_t rd = read_with_timeout(fd, buf + len, cap - len);
        if (rd < 0 && errno == EINTR) continue;
        if (rd < 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        if (rd == 0) break;
        len += (size_t)rd;
    }
    close(fd);
    buf[len] = '\0';

    unsigned char *hdr_end = NULL;
    hdr_end = (unsigned char *)strstr((char *)buf, "\r\n\r\n");
    if (!hdr_end) {
        hdr_end = (unsigned char *)strstr((char *)buf, "\n\n");
        if (hdr_end) hdr_end += 1;
    }
    if (!hdr_end) {
        free(buf);
        if (status_code) *status_code = 0;
        if (body_len) *body_len = 0;
        return NULL;
    }

    unsigned char *status_line_end = (unsigned char *)strstr((char *)buf, "\r\n");
    if (!status_line_end || status_line_end > hdr_end)
        status_line_end = (unsigned char *)strstr((char *)buf, "\n");
    if (!status_line_end) {
        free(buf);
        if (status_code) *status_code = 0;
        if (body_len) *body_len = 0;
        return NULL;
    }
    char status_line[128];
    snprintf(status_line, sizeof(status_line), "%.*s", (int)(status_line_end - buf), buf);

    int code = 0;
    {
        const char *sp = status_line;
        while (*sp && *sp != ' ') sp++;
        if (*sp == ' ') {
            sp++;
            while (*sp >= '0' && *sp <= '9') { code = code * 10 + (*sp - '0'); sp++; }
        }
    }
    if (status_code) *status_code = code;

    size_t header_len = (size_t)(hdr_end - buf);
    if (hdr_end[0] == '\r' && hdr_end[1] == '\n' && hdr_end[2] == '\r' && hdr_end[3] == '\n')
        header_len += 4;
    else
        header_len += 2;
    size_t raw_len = len - header_len;
    if (raw_len > PKG_MAX_METADATA) {
        free(buf);
        return NULL;
    }
    unsigned char *body = (unsigned char *)malloc(raw_len + 1);
    if (!body) {
        free(buf);
        dief("out of memory");
    }
    memcpy(body, buf + header_len, raw_len);
    body[raw_len] = '\0';
    if (body_len) *body_len = raw_len;

    free(buf);
    return body;
}

char *http_get_body(const char *url, int *status_code) {
    size_t body_len = 0;
    unsigned char *raw = http_get_body_raw(url, status_code, &body_len);
    if (!raw) return NULL;
    char *text = (char *)malloc(body_len + 1);
    if (!text) {
        free(raw);
        dief("out of memory");
    }
    memcpy(text, raw, body_len + 1);
    free(raw);
    return text;
}

static long parse_content_length(const unsigned char *headers, size_t header_len) {
    static const char name[] = "content-length:";
    size_t offset = 0;
    while (offset < header_len) {
        size_t end = offset;
        while (end < header_len && headers[end] != '\n') end++;
        size_t line_len = end - offset;
        if (line_len && headers[offset + line_len - 1] == '\r') line_len--;
        if (line_len >= sizeof(name) - 1 &&
            strncasecmp((const char *) headers + offset, name, sizeof(name) - 1) == 0) {
            size_t pos = offset + sizeof(name) - 1;
            while (pos < offset + line_len &&
                   (headers[pos] == ' ' || headers[pos] == '\t'))
                pos++;
            if (pos == offset + line_len) return -1;
            unsigned long value = 0;
            for (; pos < offset + line_len; pos++) {
                if (headers[pos] < '0' || headers[pos] > '9') return -1;
                if (value > PKG_MAX_DOWNLOAD) return -1;
                value = value * 10UL + (unsigned long) (headers[pos] - '0');
            }
            return value <= PKG_MAX_DOWNLOAD ? (long) value : -1;
        }
        offset = end < header_len ? end + 1 : end;
    }
    return -1;
}

static int header_has_chunked(const unsigned char *headers, size_t header_len) {
    const char *needle = "transfer-encoding:";
    for (size_t i = 0; i + strlen(needle) <= header_len; i++)
        if ((i == 0 || headers[i - 1] == '\n') &&
            strncasecmp((const char *) headers + i, needle, strlen(needle)) == 0)
            return 1;
    return 0;
}

long http_content_length(const char *url) {
    char host[256], path[1024];
    int port = 0;
    if (parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        return -1;

    int fd = connect_tcp(host, port);
    if (fd < 0) return -1;

    char request[2048];
    int request_len = make_request(request, sizeof(request), path, host);
    if (request_len < 0) {
        close(fd);
        return -1;
    }

    if (write_all(fd, request, (size_t) request_len) != 0) {
        close(fd);
        return -1;
    }

    size_t cap = 8192;
    size_t len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap + 1);
    if (!buf) { close(fd); return -1; }

    unsigned char *hdr_end = NULL;
    while (!hdr_end) {
        if (len == cap) {
            if (cap >= HTTP_MAX_HEADER) break;
            cap *= 2;
            if (cap > HTTP_MAX_HEADER) cap = HTTP_MAX_HEADER;
            unsigned char *nb = (unsigned char *)realloc(buf, cap + 1);
            if (!nb) { free(buf); close(fd); return -1; }
            buf = nb;
        }
        ssize_t rd = read_with_timeout(fd, buf + len, cap - len);
        if (rd < 0 && errno == EINTR) continue;
        if (rd <= 0) break;
        len += (size_t)rd;
        buf[len] = '\0';
        hdr_end = (unsigned char *)strstr((char *)buf, "\r\n\r\n");
        if (!hdr_end) hdr_end = (unsigned char *)strstr((char *)buf, "\n\n");
    }
    close(fd);

    long cl = -1;
    if (hdr_end) cl = parse_content_length(buf, (size_t) (hdr_end - buf));
    free(buf);
    return cl;
}

static void render_bar(const char *label, size_t received, size_t total, int bar_w) {
    int pct = 0;
    if (total > 0) pct = (int)((received * 100) / total);
    if (pct > 100) pct = 100;

    int filled = 0;
    if (total > 0) filled = (int)((received * (size_t)bar_w) / total);
    if (filled > bar_w) filled = bar_w;

    fprintf(stderr, "\r  %s%3d%%%s %s [", ANSI_BOLD, pct, ANSI_RESET, label);
    for (int i = 0; i < bar_w; i++) {
        if (i < filled) fputc('=', stderr);
        else if (i == filled) fputc('>', stderr);
        else fputc(' ', stderr);
    }
    fprintf(stderr, "]");

    if (total > 0) {
        size_t rec_k = received / 1024;
        size_t tot_k = total / 1024;
        fprintf(stderr, " %zu/%zu KB", rec_k, tot_k);
    } else {
        fprintf(stderr, " %zu bytes", received);
    }
    fflush(stderr);
}

int http_download(const char *url, const char *dest, const char *label) {
    if (verbose_mode) log_info("download %s -> %s", url, dest);
    char host[256], path[1024];
    int port = 0;
    if (parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        return -1;

    int fd = connect_tcp(host, port);
    if (fd < 0) { log_info("connect failed for %s", url); return -1; }

    char request[2048];
    int request_len = make_request(request, sizeof(request), path, host);
    if (request_len < 0) {
        close(fd);
        return -1;
    }

    if (write_all(fd, request, (size_t) request_len) != 0) {
        log_info("write failed for %s", url);
        close(fd);
        return -1;
    }

    size_t hdr_cap = 8192;
    size_t hdr_len = 0;
    unsigned char *hdr_buf = (unsigned char *)malloc(hdr_cap + 1);
    if (!hdr_buf) { close(fd); return -1; }

    unsigned char *hdr_end = NULL;
    while (!hdr_end) {
        if (hdr_len == hdr_cap) {
            if (hdr_cap >= HTTP_MAX_HEADER) {
                free(hdr_buf);
                close(fd);
                return -1;
            }
            hdr_cap *= 2;
            if (hdr_cap > HTTP_MAX_HEADER) hdr_cap = HTTP_MAX_HEADER;
            unsigned char *nb = (unsigned char *)realloc(hdr_buf, hdr_cap + 1);
            if (!nb) { free(hdr_buf); close(fd); return -1; }
            hdr_buf = nb;
        }
        ssize_t rd = read_with_timeout(fd, hdr_buf + hdr_len, hdr_cap - hdr_len);
        if (rd < 0 && errno == EINTR) continue;
        if (rd <= 0) { log_info("header read failed for %s (n=%zd)", url, rd); free(hdr_buf); close(fd); return -1; }
        hdr_len += (size_t)rd;
        hdr_buf[hdr_len] = '\0';
        hdr_end = (unsigned char *)strstr((char *)hdr_buf, "\r\n\r\n");
        if (!hdr_end)
            hdr_end = (unsigned char *)strstr((char *)hdr_buf, "\n\n");
    }

    int code = 0;
    {
        unsigned char *sl = (unsigned char *)strstr((char *)hdr_buf, "\r\n");
        if (!sl || sl > hdr_end)
            sl = (unsigned char *)strstr((char *)hdr_buf, "\n");
        if (sl) {
            const char *sp = (const char *)hdr_buf;
            while (*sp && *sp != ' ') sp++;
            if (*sp == ' ') {
                sp++;
                while (*sp >= '0' && *sp <= '9') { code = code * 10 + (*sp - '0'); sp++; }
            }
        }
    }

    size_t header_bytes = (size_t)(hdr_end - hdr_buf);
    if (hdr_end[0] == '\r' && hdr_end[1] == '\n' && hdr_end[2] == '\r' && hdr_end[3] == '\n')
        header_bytes += 4;
    else
        header_bytes += 2;
    long content_length = parse_content_length(hdr_buf, header_bytes);
    size_t initial_body = hdr_len - header_bytes;

    if (code != 200 || header_has_chunked(hdr_buf, header_bytes) ||
        (content_length >= 0 && (size_t) content_length > PKG_MAX_DOWNLOAD) ||
        initial_body > PKG_MAX_DOWNLOAD) {
        log_info("HTTP %d for %s", code, url);
        free(hdr_buf);
        close(fd);
        return -1;
    }

    int out_fd = open(dest, O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE *f = out_fd >= 0 ? fdopen(out_fd, "wb") : NULL;
    if (!f) {
        if (out_fd >= 0) close(out_fd);
        log_info("cannot create %s", dest);
        free(hdr_buf);
        close(fd);
        return -1;
    }

    size_t received = 0;
    int bar_w = 32;
    size_t expected = content_length > 0 ? (size_t) content_length : 0;
    render_bar(label, 0, expected, bar_w);

    if (initial_body > 0) {
        if (fwrite(hdr_buf + header_bytes, 1, initial_body, f) != initial_body) {
            free(hdr_buf);
            fclose(f);
            close(fd);
            unlink(dest);
            return -1;
        }
        received += initial_body;
        render_bar(label, received, expected, bar_w);
    }
    free(hdr_buf);

    size_t buf_cap = 65536;
    unsigned char *buf = (unsigned char *)malloc(buf_cap);
    if (!buf) { fclose(f); close(fd); return -1; }

    for (;;) {
        ssize_t rd = read_with_timeout(fd, buf, buf_cap);
        if (rd < 0 && errno == EINTR) continue;
        if (rd < 0) {
            log_info("body read failed for %s", url);
            free(buf);
            fclose(f);
            close(fd);
            unlink(dest);
            return -1;
        }
        if (rd == 0) break;

        size_t chunk = (size_t)rd;
        if (received > PKG_MAX_DOWNLOAD - chunk) {
            free(buf);
            fclose(f);
            close(fd);
            unlink(dest);
            log_warn("download exceeds size limit");
            return -1;
        }
        size_t written = fwrite(buf, 1, chunk, f);
        if (written != chunk) {
            log_info("fwrite failed for %s", dest);
            free(buf);
            fclose(f);
            close(fd);
            unlink(dest);
            return -1;
        }

        received += chunk;
        render_bar(label, received, expected, bar_w);
    }

    fclose(f);
    free(buf);
    close(fd);

    render_bar(label, received, expected, bar_w);
    fputc('\n', stderr);

    if (content_length > 0 && received != (size_t)content_length) {
        log_info("size mismatch for %s: expected %ld got %zu", url, content_length, received);
        unlink(dest);
        return -1;
    }

    return 0;
}

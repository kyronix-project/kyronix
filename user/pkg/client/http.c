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

#define HTTP_CONNECT_TIMEOUT_MS 2000
#define HTTP_HEADER_TIMEOUT_MS 5000
#define HTTP_BODY_TIMEOUT_MS 30000

static int parse_http_url(const char *url, char *host, size_t host_n, int *port, char *path,
                          size_t path_n) {
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
                ready = poll(&pfd, 1, HTTP_CONNECT_TIMEOUT_MS);
            } while (ready < 0 && errno == EINTR);
            if (ready > 0) {
                int socket_error = 0;
                socklen_t error_len = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0 &&
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

static struct {
    char host[256];
    int port;
    int fd;
} g_conn = { .host = "", .port = 0, .fd = -1 };

static int g_conn_reused = 0;

static void conn_close(void) {
    if (g_conn.fd >= 0) close(g_conn.fd);
    g_conn.fd = -1;
    g_conn.host[0] = '\0';
    g_conn.port = 0;
}

static int conn_get(const char *host, int port) {
    if (g_conn.fd >= 0 && g_conn.port == port && strcmp(g_conn.host, host) == 0) {
        g_conn_reused = 1;
        return g_conn.fd;
    }
    conn_close();

    int fd = connect_tcp(host, port);
    if (fd < 0) return -1;
    (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
    snprintf(g_conn.host, sizeof(g_conn.host), "%s", host);
    g_conn.port = port;
    g_conn.fd = fd;
    g_conn_reused = 0;
    return fd;
}

static ssize_t read_with_timeout(int fd, void *buf, size_t len, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready;
    do { ready = poll(&pfd, 1, timeout_ms); } while (ready < 0 && errno == EINTR);
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

static int make_request(char *out, size_t size, const char *path, const char *host, int keep_alive,
                        const char *extra) {
    int n =
        snprintf(out, size,
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: %s\r\n"
                 "Accept-Encoding: identity\r\n"
                 "%s"
                 "Connection: %s\r\n\r\n",
                 path, host, USER_AGENT, extra ? extra : "", keep_alive ? "keep-alive" : "close");
    return n >= 0 && (size_t) n < size ? n : -1;
}

typedef struct {
    int fd;
    int status;
    long content_length;
    long range_total;
    int chunked;
    int keep;
    size_t body_offset;
    unsigned char *hdr;
    size_t hdr_len;
} HttpHead;

static long parse_content_length(const unsigned char *headers, size_t header_len);
static int header_has_chunked(const unsigned char *headers, size_t header_len);

static void http_head_free(HttpHead *h) {
    free(h->hdr);
    h->hdr = NULL;
}

static long parse_content_range_total(const unsigned char *headers, size_t header_len) {
    static const char name[] = "content-range:";
    size_t offset = 0;
    while (offset < header_len) {
        size_t end = offset;
        while (end < header_len && headers[end] != '\n') { end++; }
        size_t line_len = end - offset;
        if (line_len && headers[offset + line_len - 1] == '\r') { line_len--; }
        if (line_len >= sizeof(name) - 1 &&
            strncasecmp((const char *) headers + offset, name, sizeof(name) - 1) == 0) {
            size_t pos = offset + sizeof(name) - 1;
            while (pos < offset + line_len && headers[pos] != '/') { pos++; }
            if (pos == offset + line_len) { return -1; }
            pos++;
            unsigned long value = 0;
            size_t digits = 0;
            for (; pos < offset + line_len; pos++) {
                if (headers[pos] < '0' || headers[pos] > '9') { return -1; }
                if (value > PKG_MAX_DOWNLOAD) { return -1; }
                value = value * 10UL + (unsigned long) (headers[pos] - '0');
                digits++;
            }
            return digits ? (long) value : -1;
        }
        offset = end < header_len ? end + 1 : end;
    }
    return -1;
}

static int read_header_block(int fd, HttpHead *h, size_t *bytes_read) {
    size_t cap = 8192;
    size_t len = 0;
    *bytes_read = 0;
    h->hdr = NULL;
    h->hdr_len = 0;
    unsigned char *buf = (unsigned char *) malloc(cap + 1);
    if (!buf) { dief("out of memory"); }

    unsigned char *hdr_end = NULL;
    while (!hdr_end) {
        if (len == cap) {
            if (cap >= HTTP_MAX_HEADER) {
                free(buf);
                return -1;
            }
            cap *= 2;
            if (cap > HTTP_MAX_HEADER) { cap = HTTP_MAX_HEADER; }
            unsigned char *nb = (unsigned char *) realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                dief("out of memory");
            }
            buf = nb;
        }
        ssize_t rd = read_with_timeout(fd, buf + len, cap - len, HTTP_HEADER_TIMEOUT_MS);
        if (rd < 0 && errno == EINTR) { continue; }
        if (rd <= 0) {
            *bytes_read = len;
            h->hdr_len = len;
            free(buf);
            return -1;
        }
        len += (size_t) rd;
        buf[len] = '\0';
        hdr_end = (unsigned char *) strstr((char *) buf, "\r\n\r\n");
        if (!hdr_end) { hdr_end = (unsigned char *) strstr((char *) buf, "\n\n"); }
    }

    size_t header_bytes = (size_t) (hdr_end - buf);
    if (hdr_end[0] == '\r' && hdr_end[1] == '\n' && hdr_end[2] == '\r' && hdr_end[3] == '\n') {
        header_bytes += 4;
    } else {
        header_bytes += 2;
    }

    h->status = 0;
    {
        unsigned char *sl = (unsigned char *) strstr((char *) buf, "\r\n");
        if (!sl || sl > hdr_end) { sl = (unsigned char *) strstr((char *) buf, "\n"); }
        if (sl) {
            const char *sp = (const char *) buf;
            while (*sp && *sp != ' ') { sp++; }
            if (*sp == ' ') {
                sp++;
                while (*sp >= '0' && *sp <= '9') {
                    h->status = h->status * 10 + (*sp - '0');
                    sp++;
                }
            }
        }
    }
    h->content_length = parse_content_length(buf, header_bytes);
    h->chunked = header_has_chunked(buf, header_bytes);
    h->range_total = parse_content_range_total(buf, header_bytes);
    h->hdr = buf;
    h->hdr_len = len;
    h->body_offset = header_bytes;
    h->keep = h->content_length >= 0 && !h->chunked;
    return 0;
}

static int http_head(const char *url, const char *extra, HttpHead *h) {
    memset(h, 0, sizeof(*h));
    if (!url || !starts_with(url, "http://")) {
        log_warn("invalid repository URL");
        return -1;
    }

    char host[256], path[1024];
    int port = 0;
    if (parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        log_warn("invalid repository URL");
        return -1;
    }

    char request[2048];
    int request_len = make_request(request, sizeof(request), path, host, 1, extra);
    if (request_len < 0) { return -1; }

    for (int attempt = 0; attempt < 2; attempt++) {
        int reused = g_conn.fd >= 0;

        int fd = conn_get(host, port);
        if (fd < 0) { return -1; }
        h->fd = fd;

        if (verbose_mode) { log_info("GET %s", url); }
        if (write_all(fd, request, (size_t) request_len) != 0) {
            if (reused) {
                conn_close();
                continue;
            }
            log_info("write failed for %s", url);
            conn_close();
            return -1;
        }

        size_t got = 0;
        if (read_header_block(fd, h, &got) != 0) {
            if (reused && got == 0) {
                conn_close();
                continue;
            }
            log_info("header read failed for %s", url);
            conn_close();
            return -1;
        }

        if (!h->keep) { conn_close(); }
        return 0;
    }

    conn_close();
    return -1;
}

static int read_body_exact(int fd, unsigned char *dst, size_t len, size_t *got) {
    size_t done = 0;
    while (done < len) {
        ssize_t rd = read_with_timeout(fd, dst + done, len - done, HTTP_BODY_TIMEOUT_MS);
        if (rd < 0 && errno == EINTR) { continue; }
        if (rd <= 0) {
            *got = done;
            return -1;
        }
        done += (size_t) rd;
    }
    *got = done;
    return 0;
}

unsigned char *http_get_body_raw(const char *url, int *status_code, size_t *body_len) {
    if (status_code) *status_code = 0;
    if (body_len) *body_len = 0;

    HttpHead h;
    if (http_head(url, NULL, &h) != 0) { return NULL; }
    if (status_code) *status_code = h.status;

    size_t initial = h.hdr_len - h.body_offset;
    unsigned char *body = NULL;
    size_t raw_len = 0;

    if (h.keep) {
        raw_len = (size_t) h.content_length;
        if (raw_len > PKG_MAX_METADATA) {
            http_head_free(&h);
            conn_close();
            return NULL;
        }
        body = (unsigned char *) malloc(raw_len + 1);
        if (!body) {
            http_head_free(&h);
            dief("out of memory");
        }
        if (initial > raw_len) { initial = raw_len; }
        memcpy(body, h.hdr + h.body_offset, initial);
        size_t got = initial;
        if (got < raw_len && read_body_exact(h.fd, body + got, raw_len - got, &got) != 0) {
            free(body);
            http_head_free(&h);
            conn_close();
            return NULL;
        }
        http_head_free(&h);
    } else {
        size_t cap = h.hdr_len + 65536;
        body = (unsigned char *) malloc(cap + 1);
        if (!body) {
            http_head_free(&h);
            dief("out of memory");
        }
        memcpy(body, h.hdr + h.body_offset, initial);
        raw_len = initial;
        for (;;) {
            if (raw_len == cap) {
                if (cap >= PKG_MAX_METADATA + HTTP_MAX_HEADER) {
                    free(body);
                    http_head_free(&h);
                    conn_close();
                    log_warn("metadata response exceeds size limit");
                    return NULL;
                }
                cap *= 2;
                if (cap > PKG_MAX_METADATA + HTTP_MAX_HEADER) {
                    cap = PKG_MAX_METADATA + HTTP_MAX_HEADER;
                }
                unsigned char *nb = (unsigned char *) realloc(body, cap + 1);
                if (!nb) {
                    free(body);
                    http_head_free(&h);
                    dief("out of memory");
                }
                body = nb;
            }
            ssize_t rd =
                read_with_timeout(h.fd, body + raw_len, cap - raw_len, HTTP_BODY_TIMEOUT_MS);
            if (rd < 0 && errno == EINTR) { continue; }
            if (rd < 0) {
                free(body);
                http_head_free(&h);
                conn_close();
                return NULL;
            }
            if (rd == 0) { break; }
            raw_len += (size_t) rd;
        }
        http_head_free(&h);
    }

    if (raw_len > PKG_MAX_METADATA) {
        free(body);
        return NULL;
    }
    body[raw_len] = '\0';
    if (body_len) *body_len = raw_len;
    return body;
}

char *http_get_body(const char *url, int *status_code) {
    size_t body_len = 0;
    unsigned char *raw = http_get_body_raw(url, status_code, &body_len);
    if (!raw) return NULL;
    char *text = (char *) malloc(body_len + 1);
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
            while (pos < offset + line_len && (headers[pos] == ' ' || headers[pos] == '\t')) pos++;
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
    HttpHead h;
    if (http_head(url, "Range: bytes=0-0\r\n", &h) != 0) { return -1; }

    long size = -1;
    if (h.status == 206 && h.range_total >= 0) {
        size = h.range_total;
    } else if (h.content_length >= 0) {
        size = h.content_length;
    }

    if (h.keep) { conn_close(); }
    http_head_free(&h);
    return size;
}

static void render_bar(const char *label, size_t received, size_t total, int bar_w) {
    int pct = 0;
    if (total > 0) pct = (int) ((received * 100) / total);
    if (pct > 100) pct = 100;

    int filled = 0;
    if (total > 0) filled = (int) ((received * (size_t) bar_w) / total);
    if (filled > bar_w) filled = bar_w;

    fprintf(stderr, "\r  %s%3d%%%s %s [", ANSI_BOLD, pct, ANSI_RESET, label);
    for (int i = 0; i < bar_w; i++) {
        if (i < filled)
            fputc('=', stderr);
        else if (i == filled)
            fputc('>', stderr);
        else
            fputc(' ', stderr);
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

    HttpHead h;
    if (http_head(url, NULL, &h) != 0) {
        log_info("connect failed for %s", url);
        return -1;
    }

    size_t initial_body = h.hdr_len - h.body_offset;
    if (h.status != 200 || h.chunked ||
        (h.content_length >= 0 && (size_t) h.content_length > PKG_MAX_DOWNLOAD) ||
        initial_body > PKG_MAX_DOWNLOAD) {
        log_info("HTTP %d for %s", h.status, url);
        http_head_free(&h);
        conn_close();
        return -1;
    }

    int out_fd = open(dest, O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE *f = out_fd >= 0 ? fdopen(out_fd, "wb") : NULL;
    if (!f) {
        if (out_fd >= 0) close(out_fd);
        log_info("cannot create %s", dest);
        http_head_free(&h);
        conn_close();
        return -1;
    }

    size_t received = 0;
    int bar_w = 32;
    size_t expected = h.content_length > 0 ? (size_t) h.content_length : 0;
    render_bar(label, 0, expected, bar_w);

    if (initial_body > 0) {
        if (fwrite(h.hdr + h.body_offset, 1, initial_body, f) != initial_body) {
            http_head_free(&h);
            fclose(f);
            conn_close();
            unlink(dest);
            return -1;
        }
        received += initial_body;
        render_bar(label, received, expected, bar_w);
    }
    http_head_free(&h);

    size_t buf_cap = 65536;
    unsigned char *buf = (unsigned char *) malloc(buf_cap);
    if (!buf) {
        fclose(f);
        conn_close();
        return -1;
    }

    int ok = 1;
    size_t want = SIZE_MAX;
    if (h.keep) { want = (size_t) h.content_length; }

    for (;;) {
        if (received >= want) { break; }
        size_t space = buf_cap;
        if (want != SIZE_MAX && want - received < space) { space = want - received; }
        ssize_t rd = read_with_timeout(h.fd, buf, space, HTTP_BODY_TIMEOUT_MS);
        if (rd < 0 && errno == EINTR) { continue; }
        if (rd < 0) {
            log_info("body read failed for %s", url);
            ok = 0;
            break;
        }
        if (rd == 0) {
            if (want != SIZE_MAX) {
                log_info("short body for %s", url);
                ok = 0;
            }
            break;
        }

        size_t chunk = (size_t) rd;
        if (received > PKG_MAX_DOWNLOAD - chunk) {
            log_warn("download exceeds size limit");
            ok = 0;
            break;
        }
        if (fwrite(buf, 1, chunk, f) != chunk) {
            log_info("fwrite failed for %s", dest);
            ok = 0;
            break;
        }

        received += chunk;
        render_bar(label, received, expected, bar_w);
    }

    fclose(f);
    free(buf);

    if (!ok) {
        conn_close();
        unlink(dest);
        return -1;
    }

    if (!h.keep) { conn_close(); }

    render_bar(label, received, expected, bar_w);
    fputc('\n', stderr);

    if (h.content_length > 0 && received != (size_t) h.content_length) {
        log_info("size mismatch for %s: expected %ld got %zu", url, h.content_length, received);
        conn_close();
        unlink(dest);
        return -1;
    }

    return 0;
}

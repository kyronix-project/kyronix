#define _GNU_SOURCE

#include "http.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "util.h"

static void parse_http_url(const char *url, char *host, size_t host_n, int *port, char *path, size_t path_n) {
    const char *p = NULL;
    if (starts_with(url, "http://")) {
        p = url + 7;
        *port = 80;
    } else {
        dief("only http:// URLs are supported");
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        snprintf(host, host_n, "%.*s", (int)(colon - p), p);
        { const char *np = colon + 1; int nv = 0; while (*np >= '0' && *np <= '9') { nv = nv * 10 + (*np - '0'); np++; } *port = nv; }
        if (slash) snprintf(path, path_n, "%s", slash);
        else snprintf(path, path_n, "/");
    } else {
        if (slash) {
            snprintf(host, host_n, "%.*s", (int)(slash - p), p);
            snprintf(path, path_n, "%s", slash);
        } else {
            snprintf(host, host_n, "%s", p);
            snprintf(path, path_n, "/");
        }
    }
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
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

unsigned char *http_get_body_raw(const char *url, int *status_code, size_t *body_len) {
    char host[256], path[1024];
    int port = 0;
    parse_http_url(url, host, sizeof(host), &port, path, sizeof(path));

    int fd = connect_tcp(host, port);
    if (fd < 0) dief("connection to %s:%d failed", host, port);

    char request[2048];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, USER_AGENT);

    if (verbose_mode) log_info("GET %s", url);
    if (write(fd, request, strlen(request)) < 0) {
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
            cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                close(fd);
                dief("out of memory");
            }
            buf = nb;
        }
        ssize_t rd = read(fd, buf + len, cap - len);
        if (rd < 0) {
            free(buf);
            close(fd);
            dief("network read failed");
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

static long parse_content_length(const unsigned char *headers) {
    const char *p = (const char *)headers;
    const char *needle = "content-length:";
    size_t nlen = 15;
    while (*p) {
        int match = 1;
        for (size_t i = 0; i < nlen; i++) {
            char c = p[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != needle[i]) { match = 0; break; }
        }
        if (match) {
            p += nlen;
            while (*p == ' ') p++;
            long lv = 0;
            int lneg = 1;
            if (*p == '-') { lneg = -1; p++; }
            while (*p >= '0' && *p <= '9') { lv = lv * 10 + (*p - '0'); p++; }
            return lv * lneg;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return -1;
}

long http_content_length(const char *url) {
    char host[256], path[1024];
    int port = 0;
    parse_http_url(url, host, sizeof(host), &port, path, sizeof(path));

    int fd = connect_tcp(host, port);
    if (fd < 0) return -1;

    char request[2048];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, USER_AGENT);

    if (write(fd, request, strlen(request)) < 0) {
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
            cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(buf, cap + 1);
            if (!nb) { free(buf); close(fd); return -1; }
            buf = nb;
        }
        ssize_t rd = read(fd, buf + len, cap - len);
        if (rd <= 0) break;
        len += (size_t)rd;
        buf[len] = '\0';
        hdr_end = (unsigned char *)strstr((char *)buf, "\r\n\r\n");
        if (!hdr_end) hdr_end = (unsigned char *)strstr((char *)buf, "\n\n");
    }
    close(fd);

    long cl = -1;
    if (hdr_end) cl = parse_content_length(buf);
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
    parse_http_url(url, host, sizeof(host), &port, path, sizeof(path));

    int fd = connect_tcp(host, port);
    if (fd < 0) { log_info("connect failed for %s", url); return -1; }

    char request[2048];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, USER_AGENT);

    if (write(fd, request, strlen(request)) < 0) {
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
            hdr_cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(hdr_buf, hdr_cap + 1);
            if (!nb) { free(hdr_buf); close(fd); return -1; }
            hdr_buf = nb;
        }
        ssize_t rd = read(fd, hdr_buf + hdr_len, hdr_cap - hdr_len);
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

    long content_length = parse_content_length(hdr_buf);

    size_t header_bytes = (size_t)(hdr_end - hdr_buf);
    if (hdr_end[0] == '\r' && hdr_end[1] == '\n' && hdr_end[2] == '\r' && hdr_end[3] == '\n')
        header_bytes += 4;
    else
        header_bytes += 2;
    size_t initial_body = hdr_len - header_bytes;

    if (code != 200) {
        log_info("HTTP %d for %s", code, url);
        free(hdr_buf);
        close(fd);
        return -1;
    }

    FILE *f = fopen(dest, "wb");
    if (!f) { log_info("fopen failed for %s", dest); free(hdr_buf); close(fd); return -1; }

    size_t received = 0;
    int bar_w = 32;
    render_bar(label, 0, (size_t)content_length, bar_w);

    if (initial_body > 0) {
        fwrite(hdr_buf + header_bytes, 1, initial_body, f);
        received += initial_body;
        render_bar(label, received, (size_t)content_length, bar_w);
    }
    free(hdr_buf);

    size_t buf_cap = 65536;
    unsigned char *buf = (unsigned char *)malloc(buf_cap);
    if (!buf) { fclose(f); close(fd); return -1; }

    for (;;) {
        ssize_t rd = read(fd, buf, buf_cap);
        if (rd < 0) { log_info("body read failed for %s", url); free(buf); fclose(f); close(fd); return -1; }
        if (rd == 0) break;

        size_t chunk = (size_t)rd;
        size_t written = fwrite(buf, 1, chunk, f);
        if (written != chunk) { log_info("fwrite failed for %s", dest); free(buf); fclose(f); close(fd); return -1; }

        received += chunk;
        render_bar(label, received, (size_t)content_length, bar_w);
    }

    fclose(f);
    free(buf);
    close(fd);

    render_bar(label, received, (size_t)content_length, bar_w);
    fputc('\n', stderr);

    if (content_length > 0 && received != (size_t)content_length) {
        log_info("size mismatch for %s: expected %ld got %zu", url, content_length, received);
        return -1;
    }

    return 0;
}

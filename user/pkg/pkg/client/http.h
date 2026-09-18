#ifndef PKG_HTTP_H
#define PKG_HTTP_H

#include <stddef.h>

#include "pkg.h"

unsigned char *http_get_body_raw(const char *url, int *status_code, size_t *body_len);
char *http_get_body(const char *url, int *status_code);
int http_download(const char *url, const char *dest, const char *label);
long http_content_length(const char *url);

#endif

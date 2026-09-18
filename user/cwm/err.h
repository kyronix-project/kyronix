#ifndef _ERR_H_
#define _ERR_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

static inline void err(int eval, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (fmt) {
		fprintf(stderr, "%s: ", getenv("_") ? getenv("_") : "cwm");
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, ": %s\n", strerror(errno));
	} else {
		fprintf(stderr, "%s: %s\n", getenv("_") ? getenv("_") : "cwm",
		    strerror(errno));
	}
	va_end(ap);
	exit(eval);
}

static inline void errx(int eval, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (fmt) {
		fprintf(stderr, "%s: ", getenv("_") ? getenv("_") : "cwm");
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	} else {
		fprintf(stderr, "%s: error\n", getenv("_") ? getenv("_") : "cwm");
	}
	va_end(ap);
	exit(eval);
}

static inline void warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (fmt) {
		fprintf(stderr, "%s: ", getenv("_") ? getenv("_") : "cwm");
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, ": %s\n", strerror(errno));
	} else {
		fprintf(stderr, "%s: %s\n", getenv("_") ? getenv("_") : "cwm",
		    strerror(errno));
	}
	va_end(ap);
}

static inline void warnx(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (fmt) {
		fprintf(stderr, "%s: ", getenv("_") ? getenv("_") : "cwm");
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	} else {
		fprintf(stderr, "%s: error\n", getenv("_") ? getenv("_") : "cwm");
	}
	va_end(ap);
}

#endif

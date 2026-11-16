#define _GNU_SOURCE

#include "zstd_decode.h"

#include <stdio.h>
#include <stdlib.h>

#include "../../../kernel/lib/zstd/zstddeclib.c"

int zstd_decode_file(const char *src_path, const char *dst_path) {
	FILE *in = fopen(src_path, "rb");
	if (!in) {
		return -1;
	}

	FILE *out = fopen(dst_path, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}

	ZSTD_DStream *stream = ZSTD_createDStream();
	if (!stream) {
		fclose(out);
		fclose(in);
		return -1;
	}

	size_t in_cap = ZSTD_DStreamInSize();
	size_t out_cap = ZSTD_DStreamOutSize();
	void *in_buf = malloc(in_cap);
	void *out_buf = malloc(out_cap);
	if (!in_buf || !out_buf) {
		free(out_buf);
		free(in_buf);
		ZSTD_freeDStream(stream);
		fclose(out);
		fclose(in);
		return -1;
	}

	int rc = -1;
	size_t init = ZSTD_initDStream(stream);
	if (ZSTD_isError(init)) {
		goto done;
	}

	size_t in_len = 0;
	size_t in_pos = 0;
	int eof = 0;
	for (;;) {
		if (in_pos == in_len && !eof) {
			in_len = fread(in_buf, 1, in_cap, in);
			in_pos = 0;
			if (in_len == 0) {
				if (ferror(in)) {
					goto done;
				}
				eof = 1;
			}
		}

		ZSTD_inBuffer input = { in_buf, in_len, in_pos };
		ZSTD_outBuffer output = { out_buf, out_cap, 0 };
		size_t ret = ZSTD_decompressStream(stream, &output, &input);
		if (ZSTD_isError(ret)) {
			goto done;
		}
		if (output.pos > 0 && fwrite(out_buf, 1, output.pos, out) != output.pos) {
			goto done;
		}
		in_pos = input.pos;

		if (ret == 0) {
			rc = 0;
			break;
		}

		if (eof && in_pos == in_len) {
			break;
		}
	}

done:
	if (rc == 0 && fflush(out) != 0) {
		rc = -1;
	}
	free(out_buf);
	free(in_buf);
	ZSTD_freeDStream(stream);
	if (fclose(out) != 0) {
		rc = -1;
	}
	if (fclose(in) != 0) {
		rc = -1;
	}
	return rc;
}

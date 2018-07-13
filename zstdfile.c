#if defined(__FreeBSD__)
#define _BSD_SOURCE
#else
#define _GNU_SOURCE
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#ifdef __FreeBSD__
#include <sys/endian.h>
#else
#include <bsd/sys/endian.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * We only use this for ZSTD_MAGICNUMBER, which arguably is frozen since 0.8.0
 * and should be part of the public interface:
 */
#define ZSTD_STATIC_LINKING_ONLY	1
#include <zstd.h>

#include "zstdfile.h"

#define min(a, b) ({				\
	__typeof (a) _a = (a);			\
	__typeof (b) _b = (b);			\
	_a < _b ? _a : _b; })

static cookie_read_function_t zstdfile_read;
static cookie_seek_function_t zstdfile_seek;
static cookie_close_function_t zstdfile_close;

static const cookie_io_functions_t zstdfile_io = {
	.read = zstdfile_read,
	.write = NULL,
	.seek = zstdfile_seek,
	.close = zstdfile_close,
};

#define KB (1024)
struct zstdfile {
	FILE *in;		// Source FILE stream
	uint64_t logic_offset,	// Logical offset in output (forward seeks)
		 decode_offset,	// Where we've decoded to
		 actual_len;
	uint32_t outbuf_start;

	ZSTD_DCtx *decomp;

	char *inbuf;
	char *outbuf;

	/* Tracks size of heap-allocated 'inbuf'.*/
	size_t inbuf_size;

	ZSTD_outBuffer obuf;
	/*
	 * ibuf.size tracks length of valid data in 'inbuf'.
	 * ibuf.pos tracks what the decompressor has consumed.
	 */
	ZSTD_inBuffer ibuf;

	bool eof;
};

static void
zstdfile_init(struct zstdfile *cookie)
{
	size_t res;

	assert(cookie->logic_offset == 0);
	assert(cookie->decode_offset == 0);

	cookie->actual_len = 0;

	cookie->decomp = ZSTD_createDCtx();
	if (cookie->decomp == NULL) {
		fprintf(stderr, "Failed to initialize zstd\n");
		exit(1);
	}
	res = ZSTD_initDStream(cookie->decomp);
	if (ZSTD_isError(res)) {
		fprintf(stderr, "Failed to initialize decompression stream: %s\n",
		    ZSTD_getErrorName(res));
		exit(1);
	}

	cookie->inbuf_size = ZSTD_DStreamInSize();
	cookie->ibuf.src = cookie->inbuf = malloc(cookie->inbuf_size);
	cookie->ibuf.pos = cookie->ibuf.size = 0;

	cookie->obuf.size = ZSTD_DStreamOutSize();
	cookie->obuf.dst = cookie->outbuf = malloc(cookie->obuf.size);
	cookie->obuf.pos = 0;

	if (cookie->inbuf == NULL || cookie->outbuf == NULL) {
		fprintf(stderr, "Failed to allocate buffers\n");
		exit(1);
	}

	cookie->outbuf_start = 0;
	cookie->eof = false;
}

static void
zstdfile_cleanup(struct zstdfile *cookie)
{

	ZSTD_freeDCtx(cookie->decomp);
	cookie->decomp = NULL;

	free(cookie->inbuf);
	free(cookie->outbuf);
	cookie->ibuf.src = cookie->obuf.dst = cookie->inbuf = cookie->outbuf =
	    NULL;
}

/*
 * Open zstd-compressed file 'path' as a (forward-)seekable (and rewindable),
 * read-only stream.
 *
 * If 'path' isn't a zstd file, you still get a stream.
 */
FILE *
zstdopen(const char *path, const char *mode, bool *was_zstd)
{
	unsigned char hdr[4];
	struct zstdfile *cookie;
	FILE *res, *in;
	size_t nbr;

	cookie = NULL;
	in = res = NULL;
	if (strstr(mode, "w") || strstr(mode, "a")) {
		errno = EINVAL;
		goto out;
	}

	in = fopen(path, mode);
	if (in == NULL)
		goto out;

	/* Check if file is a compressed stream: */
	nbr = fread(hdr, 1, sizeof(hdr), in);
	if (ferror(in)) {
		goto out;
	} else if (nbr < sizeof(hdr)) {
		fprintf(stderr, "File truncated\n");
		goto out;
	}

	rewind(in);

	/* If not, just return the original FILE */
	if (le32dec(hdr) != ZSTD_MAGICNUMBER) {
		*was_zstd = false;
		return (in);
	}

	cookie = malloc(sizeof(*cookie));
	if (cookie == NULL) {
		errno = ENOMEM;
		goto out;
	}

	cookie->in = in;
	cookie->logic_offset = 0;
	cookie->decode_offset = 0;

	zstdfile_init(cookie);

	res = fopencookie(cookie, mode, zstdfile_io);

out:
	if (res == NULL) {
		if (in != NULL)
			fclose(in);
		if (cookie != NULL) {
			zstdfile_cleanup(cookie);
			free(cookie);
		}
	} else
		*was_zstd = true;
	return (res);
}

// Return number of bytes into buf, 0 on EOF, -1 on error. Update
// stream offset.
static ssize_t
zstdfile_read(void *cookie_, char *buf, size_t size)
{
	struct zstdfile *cookie = cookie_;
	size_t nb, ignorebytes;
	ssize_t total = 0;
	size_t ret;

	assert(size <= SSIZE_MAX);

	if (size == 0)
		return 0;

	if (cookie->eof)
		return 0;

	ret = 0;

	ignorebytes = cookie->logic_offset - cookie->decode_offset;
	assert(ignorebytes == 0);


	do {
		size_t inflated;

		/* Drain output buffer first */
		while (cookie->obuf.pos > cookie->outbuf_start) {
			size_t left = cookie->obuf.pos - cookie->outbuf_start;
			size_t ignoreskip = min(ignorebytes, left);
			size_t toread;

			if (ignoreskip > 0) {
				ignorebytes -= ignoreskip;
				left -= ignoreskip;
				cookie->outbuf_start += ignoreskip;
				cookie->decode_offset += ignoreskip;
			}

			// Ran out of output before we seek()ed up.
			if (ignorebytes > 0)
				break;

			toread = min(left, size);
			memcpy(buf, &cookie->outbuf[cookie->outbuf_start],
			    toread);

			buf += toread;
			size -= toread;
			left -= toread;
			cookie->outbuf_start += toread;
			cookie->decode_offset += toread;
			cookie->logic_offset += toread;
			total += toread;

			if (size == 0)
				break;
		}

		if (size == 0)
			break;

		/*
		 * If we have not satisfied read, the output buffer must be
		 * empty.
		 */
		assert(cookie->obuf.pos == cookie->outbuf_start);

		/*
		 * Try to determine if we're done with all compressed input.
		 * ret == 0 is neccessary but not sufficient; it just
		 * indicates that we decoded a complete frame last time, and
		 * there will be many such frames in a core dump.
		 */
		if (ret == 0 && cookie->ibuf.pos == cookie->ibuf.size) {
			struct stat sb;
			off_t off;
			int rc;

			rc = fstat(fileno(cookie->in), &sb);
			if (rc != 0)
				err(1, "fstat");
			off = ftello(cookie->in);
			if (off < 0)
				err(1, "ftello");

			if (off == sb.st_size) {
				cookie->eof = true;
				break;
			}
		}

		/* Read more input if empty */
		if (cookie->ibuf.pos == cookie->ibuf.size) {
			nb = fread(cookie->inbuf, 1, cookie->inbuf_size,
			    cookie->in);
			if (ferror(cookie->in)) {
				warn("error read core");
				exit(1);
			}
			if (nb == 0 && feof(cookie->in)) {
				warn("truncated file");
				exit(1);
			}
			cookie->ibuf.pos = 0;
			cookie->ibuf.size = nb;
		}

		/* Reset stream state to beginning of output buffer */
		cookie->obuf.pos = 0;
		cookie->outbuf_start = 0;

		ret = ZSTD_decompressStream(cookie->decomp, &cookie->obuf,
		    &cookie->ibuf);
		if (ZSTD_isError(ret)) {
			warnx("zstd: %s (%zu)", ZSTD_getErrorName(ret), ret);
			exit(1);
		}

		inflated = cookie->obuf.pos;
		cookie->actual_len += inflated;
	} while (!ferror(cookie->in) && size > 0);

	assert(total <= SSIZE_MAX);
	return (total);
}

static int
zstdfile_seek(void *cookie_, off64_t *offset_, int whence)
{
	struct zstdfile *cookie = cookie_;
	off64_t new_offset = 0, offset = *offset_;

	if (whence == SEEK_SET) {
		new_offset = offset;
	} else if (whence == SEEK_CUR) {
		new_offset = (off64_t)cookie->logic_offset + offset;
	} else {
		/* SEEK_END not ok */
		return (-1);
	}

	if (new_offset < 0)
		return (-1);

	/* Backward seeks to anywhere but 0 are not ok */
	if (new_offset < (off64_t)cookie->logic_offset && new_offset != 0) {
		return (-1);
	}

	if (new_offset == 0) {
		/* rewind(3) */
		cookie->decode_offset = 0;
		cookie->logic_offset = 0;
		zstdfile_cleanup(cookie);
		rewind(cookie->in);
		zstdfile_init(cookie);
	} else if ((uint64_t)new_offset > cookie->logic_offset) {
		/* Emulate forward seek by skipping ... */
		char *buf;
		const size_t bsz = 32*1024;
		fprintf(stderr, "XXX Seek: Skipping %zu bytes\n",
		    (uint64_t)new_offset - cookie->logic_offset);

		buf = malloc(bsz);
		while ((uint64_t)new_offset > cookie->logic_offset) {
			size_t diff = min(bsz,
			    (uint64_t)new_offset - cookie->logic_offset);
			ssize_t err = zstdfile_read(cookie_, buf, diff);
			if (err < 0) {
				free(buf);
				return (-1);
			}

			/* Seek past EOF gets positioned at EOF */
			if (err == 0) {
				assert(cookie->eof);
				new_offset = cookie->logic_offset;
				break;
			}
		}
		free(buf);
	}

	assert(cookie->logic_offset == (uint64_t)new_offset);

	*offset_ = new_offset;
	return (0);
}

static int
zstdfile_close(void *cookie_)
{
	struct zstdfile *cookie = cookie_;

	zstdfile_cleanup(cookie);
	fclose(cookie->in);
	free(cookie);

	return 0;
}

/*
 * Zlib-FILE
 *
 * Copyright 2013 Conrad Meyer <cemeyer@uw.edu>
 *
 * Released under the terms of the MIT license; see LICENSE.
 */

#define _GNU_SOURCE

#ifdef NDEBUG
#undef NDEBUG
#endif

#ifdef __FreeBSD__
#include <sys/endian.h>
#endif
#include <sys/types.h>

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

#include "zlib.h"

#include "zfile.h"

#define GZ_HDR_SZ 10

#define min(a, b) ({				\
	__typeof (a) _a = (a);			\
	__typeof (b) _b = (b);			\
	_a < _b ? _a : _b; })

#ifndef __GLIBC__
typedef int (cookie_read_function_t)(void *, char *, int);
typedef fpos_t (cookie_seek_function_t)(void *, fpos_t, int);
typedef int (cookie_close_function_t)(void *);
#endif

static cookie_read_function_t zfile_read;
static cookie_seek_function_t zfile_seek;
static cookie_close_function_t zfile_close;

#ifdef __GLIBC__
static const cookie_io_functions_t zfile_io = {
	.read = zfile_read,
	.write = NULL,
	.seek = zfile_seek,
	.close = zfile_close,
};
#endif

#define KB (1024)
struct zfile {
	FILE *in;		// Source FILE stream
	uint64_t logic_offset,	// Logical offset in output (forward seeks)
		 decode_offset,	// Where we've decoded to
		 actual_len;
	uint32_t outbuf_start;

	z_stream decomp;

	uint8_t inbuf[32*KB];
	uint8_t outbuf[256*KB];
	bool eof;
};

static void
zfile_zlib_init(struct zfile *cookie)
{
	int rc;

	assert(cookie->logic_offset == 0);
	assert(cookie->decode_offset == 0);

	cookie->actual_len = 0;

	rc = fseeko(cookie->in, GZ_HDR_SZ, SEEK_SET);
	assert(rc == 0);

	memset(&cookie->decomp, 0, sizeof cookie->decomp);
	rc = inflateInit2(&cookie->decomp, -MAX_WBITS);
	if (rc != 0) {
		fprintf(stderr, "Failed to initialize zlib: %s\n",
		    zError(rc));
		exit(1);
	}

	cookie->decomp.next_in = NULL;
	cookie->decomp.avail_in = 0;
	cookie->decomp.next_out = cookie->outbuf;
	cookie->decomp.avail_out = sizeof cookie->outbuf;

	cookie->outbuf_start = 0;
	cookie->eof = false;
}

static void
zfile_zlib_cleanup(struct zfile *cookie)
{

	inflateEnd(&cookie->decomp);
}

/*
 * Open gzipped file 'path' as a (forward-)seekable (and rewindable), read-only
 * stream.
 *
 * If 'path' isn't a gzipped file, you still get a stream.
 */
FILE *
zopen(const char *path, const char *mode, bool *was_gzipped)
{
	static const unsigned char gz_magic[] = { 0x1f, 0x8b, 0x08 };
	unsigned char gzhdr[GZ_HDR_SZ];
	struct zfile *cookie;
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
	nbr = fread(gzhdr, 1, sizeof gzhdr, in);
	if (ferror(in)) {
		goto out;
	} else if (nbr < (sizeof gzhdr)) {
		fprintf(stderr, "File truncated\n");
		goto out;
	}
	/* If not, just return the original FILE */
	if (memcmp(gz_magic, gzhdr, sizeof gz_magic) != 0) {
		rewind(in);
		*was_gzipped = false;
		return in;
	}

	cookie = malloc(sizeof *cookie);
	if (cookie == NULL) {
		errno = ENOMEM;
		goto out;
	}

	cookie->in = in;
	cookie->logic_offset = 0;
	cookie->decode_offset = 0;
	
	zfile_zlib_init(cookie);

#ifdef __GLIBC__
	res = fopencookie(cookie, mode, zfile_io);
#else
	res = funopen(cookie, zfile_read, NULL, zfile_seek, zfile_close);
#endif

out:
	if (res == NULL) {
		if (in != NULL)
			fclose(in);
		if (cookie != NULL)
			free(cookie);
	} else
		*was_gzipped = true;
	return res;
}

// Return number of bytes into buf, 0 on EOF, -1 on error. Update
// stream offset.
static
#ifdef __GLIBC__
ssize_t
zfile_read(void *cookie_, char *buf, size_t size)
#else
int
zfile_read(void *cookie_, char *buf, int size_)
#endif
{
	struct zfile *cookie = cookie_;
	size_t nb, ignorebytes;
	ssize_t total = 0;
#ifndef __GLIBC__
	size_t size;
#endif
	int ret;

#ifdef __GLIBC__
	assert(size <= INT_MAX);
#else
	assert(size_ <= INT_MAX);
	if (size_ < 0)
		return -1;
	size = size_;
#endif

	if (size == 0)
		return 0;

	if (cookie->eof)
		return 0;

	ret = Z_OK;

	ignorebytes = cookie->logic_offset - cookie->decode_offset;
	assert(ignorebytes == 0);


	do {
		/* Drain output buffer first */
		while (cookie->decomp.next_out >
		    &cookie->outbuf[cookie->outbuf_start]) {
			size_t left = cookie->decomp.next_out -
			    &cookie->outbuf[cookie->outbuf_start];
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
		assert(cookie->decomp.next_out ==
		    &cookie->outbuf[cookie->outbuf_start]);

		if (ret == Z_STREAM_END) {
			cookie->eof = true;
			break;
		}

		/* Read more input if empty */
		if (cookie->decomp.avail_in == 0) {
			nb = fread(cookie->inbuf, 1, sizeof cookie->inbuf,
			    cookie->in);
			if (ferror(cookie->in)) {
				warn("error read core");
				exit(1);
			}
			if (nb == 0 && feof(cookie->in)) {
				warn("truncated file");
				exit(1);
			}
			cookie->decomp.avail_in = nb;
			cookie->decomp.next_in = cookie->inbuf;
		}

		/* Reset stream state to beginning of output buffer */
		cookie->decomp.next_out = cookie->outbuf;
		cookie->decomp.avail_out = sizeof cookie->outbuf;
		cookie->outbuf_start = 0;

		ret = inflate(&cookie->decomp, Z_NO_FLUSH);
		if (ret != Z_OK && ret != Z_STREAM_END) {
			warnx("inflate: %s(%d)", zError(ret), ret);
			exit(1);
		}
		cookie->actual_len +=
		    (cookie->decomp.next_out - &cookie->outbuf[0]);
	} while (!feof(cookie->in) && !ferror(cookie->in) && size > 0);

	if (cookie->eof) {
		uint32_t tlen = (uint32_t)cookie->actual_len;

		struct {
			uint32_t crc;
			uint32_t mlen;
		} gztlr;
		assert(sizeof(gztlr) == 8);

		/*
		 * Some crap follows gz stream in
		 * vmcore-mini-gzip-prioritized.0. So we have to wait until
		 * zlib reports end of stream before reading trailer, which we
		 * do here:
		 */
		size_t from_zlib_buf = min(cookie->decomp.avail_in, sizeof(gztlr));
		memcpy((char*)&gztlr, cookie->decomp.next_in, from_zlib_buf);

		/*
		 * Probably we read it all from zlib input buffer, but just in
		 * case, read anything remaining from input file.
		 */
		if (from_zlib_buf < sizeof(gztlr)) {
			size_t rem = sizeof(gztlr) - from_zlib_buf;
			size_t rd;

			while (rem > 0 && (rd =
				fread((char*)&gztlr + (sizeof(gztlr) - rem), 1, rem, cookie->in)) > 0) {
				rem -= rd;
			}

			if (rem != 0) {
				warn("core truncated");
				exit(1);
			}
		}

		/*
		 * GZ trailer is little endian. So is x86, but just for
		 * portability's sake... (also zero is same in any endian, but
		 * ...)
		 */
		gztlr.crc = le32toh(gztlr.crc);
		gztlr.mlen = le32toh(gztlr.mlen);

		if (gztlr.crc != 0) {
			warnx("CRC %08x != 0x0", gztlr.crc);
			exit(1);
		}

		if (tlen != gztlr.mlen) {
			warnx("Length %u (%zu) doesn't match gzip trailer "
			    "%u!\n", tlen, cookie->actual_len, gztlr.mlen);
			exit(1);
		}
	}

#ifndef __GLIBC__
	assert(total <= INT_MAX);
#endif
	return total;
}

static
#ifdef __GLIBC__
int
zfile_seek(void *cookie_, off64_t *offset_, int whence)
#else
fpos_t
zfile_seek(void *cookie_, fpos_t offset, int whence)
#endif
{
	struct zfile *cookie = cookie_;
#ifdef __GLIBC__
	off64_t new_offset = 0, offset = *offset_;
#else
	fpos_t new_offset = 0;
	typedef fpos_t off64_t;
#endif

	if (whence == SEEK_SET) {
		new_offset = offset;
	} else if (whence == SEEK_CUR) {
		new_offset = (off64_t)cookie->logic_offset + offset;
	} else {
		/* SEEK_END not ok */
		return -1;
	}

	if (new_offset < 0)
		return -1;

	/* Backward seeks to anywhere but 0 are not ok */
	if (new_offset < (off64_t)cookie->logic_offset && new_offset != 0) {
		return -1;
	}

	if (new_offset == 0) {
		/* rewind(3) */
		cookie->decode_offset = 0;
		cookie->logic_offset = 0;
		zfile_zlib_cleanup(cookie);
		zfile_zlib_init(cookie);
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
			ssize_t err = zfile_read(cookie_, buf, diff);
			if (err < 0) {
				free(buf);
				return -1;
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

#ifdef __GLIBC__
	*offset_ = new_offset;
	return 0;
#else
	return new_offset;
#endif
}

static int
zfile_close(void *cookie_)
{
	struct zfile *cookie = cookie_;

	zfile_zlib_cleanup(cookie);
	fclose(cookie->in);
	free(cookie);

	return 0;
}

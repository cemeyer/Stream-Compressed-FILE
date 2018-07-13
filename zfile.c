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

#define min(a, b) ({				\
	__typeof (a) _a = (a);			\
	__typeof (b) _b = (b);			\
	_a < _b ? _a : _b; })

static cookie_read_function_t zfile_read;
static cookie_seek_function_t zfile_seek;
static cookie_close_function_t zfile_close;

static const cookie_io_functions_t zfile_io = {
	.read = zfile_read,
	.write = NULL,
	.seek = zfile_seek,
	.close = zfile_close,
};

#define KB (1024)
struct zfile {
	FILE *in;		// Source FILE stream
	uint64_t logic_offset,	// Logical offset in output (forward seeks)
		 decode_offset,	// Where we've decoded to
		 actual_len;
	uint32_t outbuf_start;

	z_stream decomp;

	uint32_t crc;

	uint8_t inbuf[32*KB];
	uint8_t outbuf[256*KB];
	bool eof;
	bool truncated;
};

static void
zfile_zlib_init(struct zfile *cookie)
{
	int rc;

	cookie->logic_offset = 0;
	cookie->decode_offset = 0;
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
	cookie->truncated = false;

	cookie->crc = crc32(0, Z_NULL, 0);
}

static void
zfile_zlib_cleanup(struct zfile *cookie)
{

	inflateEnd(&cookie->decomp);
}

/*
 * Open gzipped FILE stream 'in' as a (forward-)seekable (and rewindable),
 * read-only stream.
 *
 * If 'in' isn't a gzipped file, you still get a stream (the original one is
 * returned).
 */
FILE *
zopenfile(FILE *in, const char *mode, bool *was_gzipped)
{
	unsigned char gzhdr[GZ_HDR_SZ];
	struct zfile *cookie;
	FILE *res;
	size_t nbr;

	res = NULL;
	cookie = NULL;

	if (strstr(mode, "w") || strstr(mode, "a")) {
		errno = EINVAL;
		goto out;
	}

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
		if (was_gzipped != NULL)
			*was_gzipped = false;
		return in;
	}

	cookie = malloc(sizeof *cookie);
	if (cookie == NULL) {
		errno = ENOMEM;
		goto out;
	}

	cookie->in = in;
	zfile_zlib_init(cookie);

	res = fopencookie(cookie, mode, zfile_io);

out:
	if (res == NULL) {
		if (cookie != NULL)
			zfile_zlib_cleanup(cookie);
		free(cookie);
	} else if (was_gzipped != NULL)
		*was_gzipped = true;
	return res;
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
	FILE *in, *res;

	in = fopen(path, mode);
	if (in == NULL)
		return (NULL);

	res = zopenfile(in, mode, was_gzipped);
	if (res == NULL)
		fclose(in);
	return res;
}

// Return number of bytes into buf, 0 on EOF, -1 on error. Update
// stream offset.
static ssize_t
zfile_read(void *cookie_, char *buf, size_t size)
{
	struct zfile *cookie = cookie_;
	size_t nb, ignorebytes;
	ssize_t total = 0;
	int ret;

	assert(size <= (size_t)INT_MAX);

	if (size == 0)
		return 0;

	if (cookie->eof)
		return 0;
	/*
	 * If the truncated flag is set but eof is not, we noticed the
	 * truncation after a partial read and had to return (partial) success.
	 * Proceed through the error path at 'out' to set eof flag, errno, and
	 * ferror() status on the FILE.
	 */
	if (cookie->truncated)
		goto out;

	ret = Z_OK;

	ignorebytes = cookie->logic_offset - cookie->decode_offset;
	assert(ignorebytes == 0);


	do {
		size_t inflated;

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
				/*
				 * Handle truncation errors from nested
				 * compression streams.  Could be a false
				 * positive if read(2) returned ENOBUFS
				 * instead, but I don't see any harm.
				 */
				if (errno == ENOBUFS) {
					warnx("Error reading core stream, "
					    "assuming truncated compression "
					    "stream");
					cookie->truncated = true;
					goto out;
				} else
					err(1, "error read core");
			}
			if (nb == 0 && feof(cookie->in)) {
				warnx("truncated gzip file -- no CRC to check");
				cookie->truncated = true;
				goto out;
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
		inflated = cookie->decomp.next_out - &cookie->outbuf[0];
		cookie->actual_len += inflated;
		cookie->crc = crc32(cookie->crc, cookie->outbuf, inflated);
	} while (!ferror(cookie->in) && size > 0);

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
				warnx("truncated gzip file -- lost trailer.  "
				    "No CRC to check");
				cookie->truncated = true;
				goto out;
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
			if (cookie->crc != gztlr.crc) {
				warnx("Actual CRC %08x does not match gzip "
				    "CRC %08x; this stream *may* be "
				    "corrupt. It may be worth investigating "
				    "anyway.\n", cookie->crc, gztlr.crc);
			} else
				warnx("CRC indicates this stream is good: %08x\n",
				    cookie->crc);
		}

		if (tlen != gztlr.mlen) {
			warnx("Length %u (%zu mod 2**32) doesn't match gzip trailer %u!\n",
			    tlen, cookie->actual_len, gztlr.mlen);
			exit(1);
		}
	}

out:
	assert(total <= SSIZE_MAX);
	/*
	 * If there's anything left to read, return it as a short read.
	 */
	if (total > 0)
		return (total);
	/*
	 * If the stream was truncated, report an error (which will translate
	 * into ferror() on the stream for consumers).  I checked and it seems
	 * this will work in both glibc and FreeBSD.  Basically, cookie IO
	 * functions have the same semantics as syscall IO functions, e.g.,
	 * read(2).
	 */
	if (cookie->truncated) {
		/*
		 * Other alternatives considered were EFTYPE (does not exist on
		 * Linux) or EILSEQ (confusing error string in glibc: "Invalid
		 * or incomplete multibyte or wide character").
		 */
		errno = ENOBUFS;
		cookie->eof = true;
		return (-1);
	}
	return (0);
}

static int
zfile_seek(void *cookie_, off64_t *offset, int whence)
{
	struct zfile *cookie = cookie_;
	off64_t new_offset = 0;

	if (whence == SEEK_SET) {
		new_offset = *offset;
	} else if (whence == SEEK_CUR) {
		new_offset = (off64_t)cookie->logic_offset + *offset;
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
		zfile_zlib_cleanup(cookie);
		rewind(cookie->in);
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
	*offset = new_offset;

	return 0;
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

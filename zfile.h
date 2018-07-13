/*
 * Zlib-FILE
 *
 * Copyright 2013 Conrad Meyer <cemeyer@uw.edu>
 *
 * Released under the terms of the MIT license; see LICENSE.
 */

#ifndef ZFILE_H
#define ZFILE_H

#define GZ_HDR_SZ 10

static const unsigned char gz_magic[] = { 0x1f, 0x8b, 0x08 };

FILE *zopen(const char *path, const char *mode, bool *was_gzipped);
FILE *zopenfile(FILE *f, const char *mode, bool *was_gzipped);

#endif

/*
 * Zlib-FILE
 *
 * Copyright 2013 Conrad Meyer <cemeyer@uw.edu>
 *
 * Released under the terms of the MIT license; see LICENSE.
 */

#ifndef ZFILE_H
#define ZFILE_H

FILE *zopen(const char *path, const char *mode, bool *was_gzipped);

#endif

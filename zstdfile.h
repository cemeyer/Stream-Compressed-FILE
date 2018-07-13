#pragma once

FILE *zstdopen(const char *path, const char *mode, bool *was_zstd);
FILE *zstdopenfile(FILE *in, const char *mode, bool *was_zstd);

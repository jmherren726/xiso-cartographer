#ifndef XCAT_TEST_FILE_SOURCE_H
#define XCAT_TEST_FILE_SOURCE_H

/* A stdio-backed xc_source for host tests (64-bit offsets, images over 4 GiB). */

#include <stdio.h>

#include "xcat/source.h"

typedef struct test_file_source {
    FILE *f;
} test_file_source;

/* Opens `path` read-only and fills `src`. `fs` must outlive `src`. */
xc_err test_file_source_open(xc_source *src, test_file_source *fs, const char *path);
void test_file_source_close(test_file_source *fs);

#endif /* XCAT_TEST_FILE_SOURCE_H */

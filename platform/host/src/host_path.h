#ifndef XCAT_HOST_PATH_H
#define XCAT_HOST_PATH_H

/* UTF-8 path helpers shared by the host adapters (see xcat/host.h). Internal. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "xcat/host.h" /* xc_host_fopen, xc_host_remove */

/* Creates one directory; an existing directory counts as success. */
bool xc_host_mkdir(const char *path);
/* True if a file or directory exists. */
bool xc_host_exists(const char *path);
/* Free bytes on the drive holding `path`, or its nearest existing parent. */
bool xc_host_dir_free_space(const char *path, uint64_t *bytes);
/* Flushes a stream to the device (survives a power cut, not only a crash). */
bool xc_host_sync(FILE *f);
/* Cuts a file opened for update back to `keep` bytes; false if it's shorter than that. */
bool xc_host_truncate(FILE *f, uint64_t keep);

#endif /* XCAT_HOST_PATH_H */

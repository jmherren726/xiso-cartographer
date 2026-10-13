#ifndef XCAT_HOST_PATH_H
#define XCAT_HOST_PATH_H

/* UTF-8 path helpers shared by the host adapters (see xcat/host.h). Internal. */

#include <stdbool.h>
#include <stdio.h>

/* fopen with a UTF-8 path; long paths work on Windows. */
FILE *xc_host_fopen(const char *path, const char *mode);
/* Creates one directory; an existing directory counts as success. */
bool xc_host_mkdir(const char *path);
/* Deletes a file; returns true on success. */
bool xc_host_remove(const char *path);

#endif /* XCAT_HOST_PATH_H */

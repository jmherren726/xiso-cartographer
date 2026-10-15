#ifndef XCAT_HOST_PATH_H
#define XCAT_HOST_PATH_H

/* UTF-8 path helpers shared by the host adapters (see xcat/host.h). Internal. */

#include <stdbool.h>
#include <stdio.h>

#include "xcat/host.h" /* xc_host_fopen, xc_host_remove */

/* Creates one directory; an existing directory counts as success. */
bool xc_host_mkdir(const char *path);

#endif /* XCAT_HOST_PATH_H */

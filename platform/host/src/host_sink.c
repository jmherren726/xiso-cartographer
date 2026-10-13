#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_path.h"
#include "xcat/host.h"

/* root + "/" + rel, or NULL. Caller frees. */
static char *join(const char *root, const char *rel)
{
    size_t size = strlen(root) + 1 + strlen(rel) + 1;
    char *p = malloc(size);
    if (p == NULL) {
        return NULL;
    }
    int n = snprintf(p, size, "%s/%s", root, rel);
    if (n < 0 || (size_t)n >= size) {
        free(p);
        return NULL;
    }
    return p;
}

static xc_err sink_make_dir(void *ctx, const char *path)
{
    xc_host_dir_sink *ds = ctx;
    char *full = join(ds->root, path);
    if (full == NULL) {
        return XC_ERR_NOMEM;
    }
    bool ok = xc_host_mkdir(full);
    free(full);
    return ok ? XC_OK : XC_ERR_IO;
}

static xc_err sink_begin_file(void *ctx, const char *path, uint32_t size)
{
    xc_host_dir_sink *ds = ctx;
    if (ds->f != NULL) {
        return XC_ERR_INVALID_ARG; /* one file at a time */
    }
    ds->open_path = join(ds->root, path);
    if (ds->open_path == NULL) {
        return XC_ERR_NOMEM;
    }
    ds->f = xc_host_fopen(ds->open_path, "wb");
    if (ds->f == NULL) {
        free(ds->open_path);
        ds->open_path = NULL;
        return XC_ERR_IO;
    }
    ds->expected = size;
    ds->written = 0;
    return XC_OK;
}

static xc_err sink_write(void *ctx, const void *buf, size_t len)
{
    xc_host_dir_sink *ds = ctx;
    if (ds->f == NULL || (uint64_t)len > (uint64_t)ds->expected - ds->written) {
        return XC_ERR_INVALID_ARG; /* never write past the declared size */
    }
    if (fwrite(buf, 1, len, ds->f) != len) {
        return XC_ERR_IO;
    }
    ds->written += len;
    return XC_OK;
}

static xc_err sink_end_file(void *ctx, bool complete)
{
    xc_host_dir_sink *ds = ctx;
    if (ds->f == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    bool ok = fclose(ds->f) == 0;
    ds->f = NULL;
    ok = ok && complete && ds->written == ds->expected;
    if (!ok) {
        (void)xc_host_remove(ds->open_path); /* no partial files left behind */
    }
    free(ds->open_path);
    ds->open_path = NULL;
    if (!complete) {
        return XC_OK;
    }
    return ok ? XC_OK : XC_ERR_IO;
}

xc_err xc_host_dir_sink_open(xc_sink *sink, xc_host_dir_sink *ds, const char *root)
{
    if (sink == NULL || ds == NULL || root == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(ds, 0, sizeof *ds);
    size_t n = strlen(root);
    while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\')) {
        n--; /* join() adds the separator */
    }
    ds->root = malloc(n + 1);
    if (ds->root == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(ds->root, root, n);
    ds->root[n] = '\0';
    /* Like `mkdir -p`: create missing parents (failures there, e.g. on "C:", are fine as long
       as the root itself can be created or already exists). */
    for (size_t i = 1; i < n; i++) {
        if (ds->root[i] == '/' || ds->root[i] == '\\') {
            char sep = ds->root[i];
            ds->root[i] = '\0';
            (void)xc_host_mkdir(ds->root);
            ds->root[i] = sep;
        }
    }
    if (!xc_host_mkdir(ds->root)) {
        xc_host_dir_sink_close(ds);
        return XC_ERR_IO;
    }
    sink->make_dir = sink_make_dir;
    sink->begin_file = sink_begin_file;
    sink->write = sink_write;
    sink->end_file = sink_end_file;
    sink->ctx = ds;
    return XC_OK;
}

void xc_host_dir_sink_close(xc_host_dir_sink *ds)
{
    if (ds == NULL) {
        return;
    }
    if (ds->f != NULL) {
        (void)sink_end_file(ds, false);
    }
    free(ds->root);
    memset(ds, 0, sizeof *ds);
}

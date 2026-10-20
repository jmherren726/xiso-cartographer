/* xc_install_io (xcat/install.h) for PCs: plain files, UTF-8 and long paths (host_path.c). */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "host_path.h"
#include "xcat/host.h"

static bool io_exists(void *ctx, const char *path)
{
    (void)ctx;
    return xc_host_exists(path);
}

static xc_err io_file_size(void *ctx, const char *path, uint64_t *size)
{
    (void)ctx;
    xc_host_file hf = {NULL};
    xc_source src;
    xc_err err = xc_host_file_open(&src, &hf, path);
    if (err == XC_OK) {
        *size = src.size;
        xc_host_file_close(&hf);
    }
    return err;
}

static xc_err io_read_all(void *ctx, const char *path, size_t max_len, char **data, size_t *len)
{
    (void)ctx;
    FILE *f = xc_host_fopen(path, "rb");
    if (f == NULL) {
        return XC_ERR_IO;
    }
    char *buf = malloc(max_len + 1);
    size_t n = buf != NULL ? fread(buf, 1, max_len + 1, f) : 0;
    bool ok = buf != NULL && ferror(f) == 0;
    (void)fclose(f);
    if (!ok || n > max_len) {
        xc_err err = buf == NULL ? XC_ERR_NOMEM : !ok ? XC_ERR_IO : XC_ERR_LIMIT;
        free(buf);
        return err;
    }
    *data = buf;
    *len = n;
    return XC_OK;
}

static xc_err write_mode(const char *path, const char *mode, const void *data, size_t len)
{
    FILE *f = xc_host_fopen(path, mode);
    if (f == NULL) {
        return XC_ERR_IO;
    }
    bool ok = fwrite(data, 1, len, f) == len && xc_host_sync(f);
    ok = fclose(f) == 0 && ok;
    return ok ? XC_OK : XC_ERR_IO;
}

static xc_err io_append(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    return write_mode(path, "ab", data, len);
}

static xc_err io_write_all(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    return write_mode(path, "wb", data, len);
}

static xc_err io_remove(void *ctx, const char *path)
{
    (void)ctx;
    return xc_host_remove(path) ? XC_OK : XC_ERR_IO;
}

static xc_err io_free_space(void *ctx, const char *dir, uint64_t *bytes)
{
    (void)ctx;
    return xc_host_dir_free_space(dir, bytes) ? XC_OK : XC_ERR_IO;
}

static xc_err io_open_sink(void *ctx, const char *root, xc_sink *sink, void **handle)
{
    (void)ctx;
    xc_host_dir_sink *ds = malloc(sizeof *ds);
    if (ds == NULL) {
        return XC_ERR_NOMEM;
    }
    xc_err err = xc_host_dir_sink_open(sink, ds, root);
    if (err != XC_OK) {
        free(ds);
        return err;
    }
    *handle = ds;
    return XC_OK;
}

static void io_close_sink(void *ctx, void *handle)
{
    (void)ctx;
    if (handle != NULL) {
        xc_host_dir_sink_close(handle);
        free(handle);
    }
}

static xc_err io_open_file(void *ctx, const char *path, xc_source *src, void **handle)
{
    (void)ctx;
    xc_host_file *hf = malloc(sizeof *hf);
    if (hf == NULL) {
        return XC_ERR_NOMEM;
    }
    hf->f = NULL;
    xc_err err = xc_host_file_open(src, hf, path);
    if (err != XC_OK) {
        free(hf);
        return err;
    }
    *handle = hf;
    return XC_OK;
}

static void io_close_file(void *ctx, void *handle)
{
    (void)ctx;
    if (handle != NULL) {
        xc_host_file_close(handle);
        free(handle);
    }
}

/*
 * Spool files (xcat/install.h): one is kept open between calls, since a Deflate zip install
 * appends to the same chunk many times in a row. A file that was appended to is flushed to
 * disk when another is opened, so spool_sync only has the open one left to do
 * (stream-install.md §7.1). One install at a time per process.
 */
static struct {
    char path[4096];
    FILE *f;
    bool dirty; /* appended to since it was last flushed to disk */
} spool;

static bool spool_close(void)
{
    bool ok = true;
    if (spool.f != NULL) {
        ok = !spool.dirty || xc_host_sync(spool.f);
        ok = fclose(spool.f) == 0 && ok;
        spool.f = NULL;
    }
    spool.dirty = false;
    spool.path[0] = '\0';
    return ok;
}

static FILE *spool_open(const char *path)
{
    if (spool.f != NULL && strcmp(spool.path, path) == 0) {
        return spool.f;
    }
    if (!spool_close() || strlen(path) >= sizeof spool.path) {
        return NULL;
    }
    spool.f = xc_host_fopen(path, "a+b");
    if (spool.f != NULL) {
        memcpy(spool.path, path, strlen(path) + 1);
    }
    return spool.f;
}

static xc_err io_spool_append(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    FILE *f = spool_open(path);
    if (f == NULL || fseek(f, 0, SEEK_END) != 0) {
        return XC_ERR_IO;
    }
    spool.dirty = true;
    return fwrite(data, 1, len, f) == len ? XC_OK : XC_ERR_IO;
}

static xc_err io_spool_read(void *ctx, const char *path, uint64_t pos, void *buf, size_t len)
{
    (void)ctx;
    FILE *f = spool_open(path);
    if (f == NULL || pos > (uint64_t)LONG_MAX || fflush(f) != 0 ||
        fseek(f, (long)pos, SEEK_SET) != 0) {
        return XC_ERR_IO;
    }
    return fread(buf, 1, len, f) == len ? XC_OK : XC_ERR_IO;
}

static xc_err io_spool_remove(void *ctx, const char *path)
{
    (void)ctx;
    if (strcmp(spool.path, path) == 0) {
        spool.dirty = false; /* going anyway */
        (void)spool_close();
    }
    return xc_host_remove(path) ? XC_OK : XC_ERR_IO;
}

static xc_err io_spool_sync(void *ctx)
{
    (void)ctx;
    if (spool.f == NULL || !spool.dirty) {
        return XC_OK;
    }
    if (!xc_host_sync(spool.f)) {
        return XC_ERR_IO;
    }
    spool.dirty = false;
    return XC_OK;
}

void xc_host_install_io(xc_install_io *io)
{
    memset(io, 0, sizeof *io);
#if defined(_WIN32)
    io->separator = '\\';
#else
    io->separator = '/';
#endif
    io->exists = io_exists;
    io->file_size = io_file_size;
    io->read_all = io_read_all;
    io->append = io_append;
    io->write_all = io_write_all;
    io->remove = io_remove;
    io->free_space = io_free_space;
    io->open_sink = io_open_sink;
    io->close_sink = io_close_sink;
    io->open_file = io_open_file;
    io->close_file = io_close_file;
    io->spool_append = io_spool_append;
    io->spool_read = io_spool_read;
    io->spool_remove = io_spool_remove;
    io->spool_sync = io_spool_sync;
}

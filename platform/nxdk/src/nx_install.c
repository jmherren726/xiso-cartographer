/*
 * xc_install_io (xcat/install.h) for the Xbox: nxdk's Win32-style file API on FATX.
 * "Durable" is NtFlushBuffersFile before the handle is closed, so a journal line or a
 * game file survives the power switch (docs/formats/journal.md §2). Spool files for Deflate
 * zips are flushed when the install asks (stream-install.md §7.1).
 */
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xcat/nxdk.h"

static bool io_exists(void *ctx, const char *path)
{
    (void)ctx;
    return strlen(path) < XC_NX_PATH_MAX && xc_nx_exists(path);
}

static xc_err io_file_size(void *ctx, const char *path, uint64_t *size)
{
    (void)ctx;
    xc_nx_file f = {NULL};
    xc_source src;
    xc_err err = xc_nx_file_open(&src, &f, path);
    if (err == XC_OK) {
        *size = src.size;
        xc_nx_file_close(&f);
    }
    return err;
}

static xc_err io_read_all(void *ctx, const char *path, size_t max_len, char **data, size_t *len)
{
    (void)ctx;
    xc_nx_file f = {NULL};
    xc_source src;
    xc_err err = xc_nx_file_open(&src, &f, path);
    if (err != XC_OK) {
        return err;
    }
    if (src.size > max_len) {
        xc_nx_file_close(&f);
        return XC_ERR_LIMIT;
    }
    char *buf = malloc((size_t)src.size + 1);
    err = buf != NULL ? xc_source_read(&src, 0, buf, (size_t)src.size) : XC_ERR_NOMEM;
    xc_nx_file_close(&f);
    if (err != XC_OK) {
        free(buf);
        return err;
    }
    *data = buf;
    *len = (size_t)src.size;
    return XC_OK;
}

/* Writes `data` at the end (append) or over the file (replace), then flushes it. */
static xc_err write_file(const char *path, bool append, const void *data, size_t len)
{
    if (strlen(path) >= XC_NX_PATH_MAX) {
        return XC_ERR_LIMIT;
    }
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return XC_ERR_IO;
    }
    bool ok = !append || SetFilePointer(h, 0, NULL, FILE_END) != INVALID_SET_FILE_POINTER;
    const uint8_t *p = data;
    while (ok && len > 0) {
        DWORD put = 0;
        DWORD chunk = len > 0x40000000U ? 0x40000000U : (DWORD)len;
        ok = WriteFile(h, p, chunk, &put, NULL) && put == chunk;
        p += put;
        len -= put;
    }
    IO_STATUS_BLOCK io;
    ok = ok && NT_SUCCESS(NtFlushBuffersFile(h, &io));
    ok = CloseHandle(h) != 0 && ok;
    return ok ? XC_OK : XC_ERR_IO;
}

static xc_err io_append(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    return write_file(path, true, data, len);
}

static xc_err io_write_all(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    return write_file(path, false, data, len);
}

static xc_err io_remove(void *ctx, const char *path)
{
    (void)ctx;
    return DeleteFileA(path) ? XC_OK : XC_ERR_IO;
}

static xc_err io_free_space(void *ctx, const char *dir, uint64_t *bytes)
{
    (void)ctx;
    /* The drive's root, "E:\" for "E:\Games". */
    char root[4] = {dir[0], ':', '\\', '\0'};
    return xc_nx_free_space(root, bytes);
}

static xc_err io_open_sink(void *ctx, const char *root, xc_sink *sink, void **handle)
{
    (void)ctx;
    xc_nx_dir_sink *ds = malloc(sizeof *ds);
    if (ds == NULL) {
        return XC_ERR_NOMEM;
    }
    xc_err err = xc_nx_dir_sink_open(sink, ds, root);
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
        xc_nx_dir_sink_close(handle);
        free(handle);
    }
}

static xc_err io_open_file(void *ctx, const char *path, xc_source *src, void **handle)
{
    (void)ctx;
    xc_nx_file *f = malloc(sizeof *f);
    if (f == NULL) {
        return XC_ERR_NOMEM;
    }
    f->handle = NULL;
    xc_err err = xc_nx_file_open(src, f, path);
    if (err != XC_OK) {
        free(f);
        return err;
    }
    *handle = f;
    return XC_OK;
}

static void io_close_file(void *ctx, void *handle)
{
    (void)ctx;
    if (handle != NULL) {
        xc_nx_file_close(handle);
        free(handle);
    }
}

/*
 * Spool files for Deflate zip installs (docs/formats/stream-install.md §4): one stays open
 * between calls, since the install appends to the same chunk many times in a row. A file
 * that was appended to is flushed to disk before another is opened, so spool_sync only has
 * the open one left to do (§7.1). One install at a time.
 */
static struct {
    char path[XC_NX_PATH_MAX];
    HANDLE h;
    bool dirty; /* appended to since it was last flushed */
} spool = {"", INVALID_HANDLE_VALUE, false};

static bool spool_flush(void)
{
    if (spool.h == INVALID_HANDLE_VALUE || !spool.dirty) {
        return true;
    }
    IO_STATUS_BLOCK io;
    spool.dirty = false;
    return NT_SUCCESS(NtFlushBuffersFile(spool.h, &io));
}

static bool spool_close(void)
{
    bool ok = spool_flush();
    if (spool.h != INVALID_HANDLE_VALUE) {
        ok = CloseHandle(spool.h) != 0 && ok;
        spool.h = INVALID_HANDLE_VALUE;
    }
    spool.path[0] = '\0';
    return ok;
}

static HANDLE spool_open(const char *path)
{
    if (spool.h != INVALID_HANDLE_VALUE && strcmp(spool.path, path) == 0) {
        return spool.h;
    }
    if (!spool_close() || strlen(path) >= sizeof spool.path) {
        return INVALID_HANDLE_VALUE;
    }
    spool.h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (spool.h != INVALID_HANDLE_VALUE) {
        memcpy(spool.path, path, strlen(path) + 1);
    }
    return spool.h;
}

static xc_err io_spool_append(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    HANDLE h = spool_open(path);
    if (h == INVALID_HANDLE_VALUE ||
        SetFilePointer(h, 0, NULL, FILE_END) == INVALID_SET_FILE_POINTER) {
        return XC_ERR_IO;
    }
    spool.dirty = true;
    const uint8_t *p = data;
    while (len > 0) {
        DWORD put = 0;
        DWORD chunk = len > 0x40000000U ? 0x40000000U : (DWORD)len;
        if (!WriteFile(h, p, chunk, &put, NULL) || put != chunk) {
            return XC_ERR_IO;
        }
        p += put;
        len -= put;
    }
    return XC_OK;
}

static xc_err io_spool_read(void *ctx, const char *path, uint64_t pos, void *buf, size_t len)
{
    (void)ctx;
    HANDLE h = spool_open(path);
    LONG hi = (LONG)(pos >> 32U);
    if (h == INVALID_HANDLE_VALUE ||
        (SetFilePointer(h, (LONG)(uint32_t)pos, &hi, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
         GetLastError() != NO_ERROR)) {
        return XC_ERR_IO;
    }
    uint8_t *p = buf;
    while (len > 0) {
        DWORD got = 0;
        DWORD chunk = len > 0x40000000U ? 0x40000000U : (DWORD)len;
        if (!ReadFile(h, p, chunk, &got, NULL) || got == 0) {
            return XC_ERR_IO;
        }
        p += got;
        len -= got;
    }
    return XC_OK;
}

static xc_err io_spool_remove(void *ctx, const char *path)
{
    (void)ctx;
    if (strcmp(spool.path, path) == 0) {
        spool.dirty = false; /* going anyway */
        (void)spool_close();
    }
    return DeleteFileA(path) ? XC_OK : XC_ERR_IO;
}

static xc_err io_spool_sync(void *ctx)
{
    (void)ctx;
    return spool_flush() ? XC_OK : XC_ERR_IO;
}

void xc_nx_spool_close(void)
{
    (void)spool_close();
}

void xc_nx_install_io(xc_install_io *io)
{
    memset(io, 0, sizeof *io);
    io->separator = '\\';
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

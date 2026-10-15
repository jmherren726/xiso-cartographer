/*
 * Xbox (nxdk) platform adapters; see xcat/nxdk.h. Uses nxdk's Win32-style file
 * API, whose paths must stay below 260 bytes (it asserts that). That always
 * holds here, because FATX itself stops at 250 after the drive.
 */
#include <nxdk/mount.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xcat/nxdk.h"

/* --- source ------------------------------------------------------------------ */

static xc_err file_read_at(void *ctx, uint64_t offset, void *buf, size_t len)
{
    xc_nx_file *f = ctx;
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f->handle, pos, NULL, FILE_BEGIN)) {
        return XC_ERR_IO;
    }
    uint8_t *out = buf;
    while (len > 0) {
        DWORD chunk = len > 0x40000000U ? 0x40000000U : (DWORD)len;
        DWORD got = 0;
        if (!ReadFile(f->handle, out, chunk, &got, NULL) || got != chunk) {
            return XC_ERR_IO;
        }
        out += got;
        len -= got;
    }
    return XC_OK;
}

xc_err xc_nx_file_open(xc_source *src, xc_nx_file *f, const char *path)
{
    if (src == NULL || f == NULL || path == NULL || strlen(path) >= XC_NX_PATH_MAX) {
        return XC_ERR_INVALID_ARG;
    }
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return XC_ERR_IO;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        CloseHandle(h);
        return XC_ERR_IO;
    }
    f->handle = h;
    src->read_at = file_read_at;
    src->read_stream = NULL;
    src->ctx = f;
    src->size = (uint64_t)size.QuadPart;
    return XC_OK;
}

void xc_nx_file_close(xc_nx_file *f)
{
    if (f != NULL && f->handle != NULL) {
        CloseHandle(f->handle);
        f->handle = NULL;
    }
}

/* --- sink ---------------------------------------------------------------------- */

static const char *last_op;
static unsigned long last_code;

static void io_failed(const char *op, unsigned long code)
{
    last_op = op;
    last_code = code;
}

void xc_nx_last_io_error(const char **op, unsigned long *code)
{
    *op = last_op;
    *code = last_code;
}

/* root + "\" + rel (with '/' -> '\'); false if it doesn't fit. */
static bool join(char *out, const char *root, const char *rel)
{
    int n = snprintf(out, XC_NX_PATH_MAX, "%s\\%s", root, rel);
    if (n < 0 || n >= XC_NX_PATH_MAX) {
        return false;
    }
    for (char *p = out; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    return true;
}

static xc_err sink_make_dir(void *ctx, const char *path)
{
    xc_nx_dir_sink *ds = ctx;
    char full[XC_NX_PATH_MAX];
    if (!join(full, ds->root, path)) {
        return XC_ERR_LIMIT;
    }
    if (CreateDirectoryA(full, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return XC_OK;
    }
    return XC_ERR_IO;
}

static xc_err sink_begin_file(void *ctx, const char *path, uint32_t size)
{
    xc_nx_dir_sink *ds = ctx;
    if (ds->handle != NULL) {
        return XC_ERR_INVALID_ARG;
    }
    if (!join(ds->open_path, ds->root, path)) {
        return XC_ERR_LIMIT;
    }
    HANDLE h = CreateFileA(ds->open_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        io_failed("create", (unsigned long)GetLastError());
        return XC_ERR_IO;
    }
    ds->handle = h;
    ds->expected = size;
    ds->written = 0;
    return XC_OK;
}

static xc_err sink_write(void *ctx, const void *buf, size_t len)
{
    xc_nx_dir_sink *ds = ctx;
    if (ds->handle == NULL || (uint64_t)len > (uint64_t)ds->expected - ds->written) {
        return XC_ERR_INVALID_ARG;
    }
    const uint8_t *p = buf;
    while (len > 0) {
        DWORD chunk = len > 0x40000000U ? 0x40000000U : (DWORD)len;
        DWORD put = 0;
        if (!WriteFile(ds->handle, p, chunk, &put, NULL) || put != chunk) {
            io_failed("write", (unsigned long)GetLastError());
            return XC_ERR_IO; /* e.g. disk full */
        }
        p += put;
        len -= put;
        ds->written += put;
    }
    return XC_OK;
}

static xc_err sink_end_file(void *ctx, bool complete)
{
    xc_nx_dir_sink *ds = ctx;
    if (ds->handle == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    /* A completed file is on the disk before end_file returns: the install journal records it
       next (docs/formats/journal.md §2.3). */
    IO_STATUS_BLOCK io;
    NTSTATUS flushed = complete ? NtFlushBuffersFile(ds->handle, &io) : STATUS_SUCCESS;
    bool synced = NT_SUCCESS(flushed);
    if (!synced) {
        io_failed("flush", (unsigned long)flushed);
    }
    bool closed = CloseHandle(ds->handle) != 0;
    if (!closed && synced) {
        io_failed("close", (unsigned long)GetLastError());
    }
    bool ok = closed && synced;
    ds->handle = NULL;
    ok = ok && complete && ds->written == ds->expected;
    if (!ok) {
        DeleteFileA(ds->open_path); /* never leave a partial file behind */
    }
    if (!complete) {
        return XC_OK;
    }
    return ok ? XC_OK : XC_ERR_IO;
}

/* ADR 0011: a checkpoint needs the bytes so far on the disk. */
static xc_err sink_sync(void *ctx)
{
    xc_nx_dir_sink *ds = ctx;
    if (ds->handle == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    IO_STATUS_BLOCK io;
    NTSTATUS st = NtFlushBuffersFile(ds->handle, &io);
    if (!NT_SUCCESS(st)) {
        io_failed("flush", (unsigned long)st);
        return XC_ERR_IO;
    }
    return XC_OK;
}

/* ADR 0011: reopens a partly written file, cut back to its last checkpoint. */
static xc_err sink_resume_file(void *ctx, const char *path, uint32_t size, uint32_t keep)
{
    xc_nx_dir_sink *ds = ctx;
    if (ds->handle != NULL || keep > size) {
        return XC_ERR_INVALID_ARG;
    }
    if (!join(ds->open_path, ds->root, path)) {
        return XC_ERR_LIMIT;
    }
    HANDLE h = CreateFileA(ds->open_path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        io_failed("reopen", (unsigned long)GetLastError());
        return XC_ERR_IO;
    }
    LARGE_INTEGER have;
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)keep;
    if (!GetFileSizeEx(h, &have) || have.QuadPart < (LONGLONG)keep ||
        !SetFilePointerEx(h, at, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
        io_failed("truncate", (unsigned long)GetLastError());
        CloseHandle(h);
        return XC_ERR_IO;
    }
    ds->handle = h;
    ds->expected = size;
    ds->written = keep;
    return XC_OK;
}

/* ADR 0011: closes an unfinished file that has a checkpoint, keeping it for the next attempt. */
static xc_err sink_suspend_file(void *ctx)
{
    xc_nx_dir_sink *ds = ctx;
    if (ds->handle == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    bool ok = CloseHandle(ds->handle) != 0;
    ds->handle = NULL;
    return ok ? XC_OK : XC_ERR_IO;
}

xc_err xc_nx_dir_sink_open(xc_sink *sink, xc_nx_dir_sink *ds, const char *root)
{
    if (sink == NULL || ds == NULL || root == NULL || strlen(root) >= XC_NX_PATH_MAX) {
        return XC_ERR_INVALID_ARG;
    }
    memset(ds, 0, sizeof *ds);
    memcpy(ds->root, root, strlen(root) + 1);
    if (!xc_nx_mkdirs(ds->root)) {
        return XC_ERR_IO;
    }
    sink->make_dir = sink_make_dir;
    sink->begin_file = sink_begin_file;
    sink->write = sink_write;
    sink->end_file = sink_end_file;
    sink->ctx = ds;
    sink->sync = sink_sync;
    sink->resume_file = sink_resume_file;
    sink->suspend_file = sink_suspend_file;
    return XC_OK;
}

void xc_nx_dir_sink_close(xc_nx_dir_sink *ds)
{
    if (ds != NULL && ds->handle != NULL) {
        (void)sink_end_file(ds, false);
    }
}

/* --- drives ---------------------------------------------------------------------- */

bool xc_nx_mount_e(void)
{
    return nxIsDriveMounted('E') || nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
}

xc_err xc_nx_free_space(const char *path, uint64_t *free_bytes)
{
    ULARGE_INTEGER avail;
    if (path == NULL || free_bytes == NULL || !GetDiskFreeSpaceExA(path, &avail, NULL, NULL)) {
        return XC_ERR_IO;
    }
    *free_bytes = avail.QuadPart;
    return XC_OK;
}

bool xc_nx_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool xc_nx_mkdirs(const char *path)
{
    char buf[XC_NX_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof buf) {
        return false;
    }
    memcpy(buf, path, n + 1);
    /* Create each parent after the drive ("E:\a", "E:\a\b", ...), then the path. */
    for (size_t i = 3; i < n; i++) {
        if (buf[i] == '\\') {
            buf[i] = '\0';
            CreateDirectoryA(buf, NULL);
            buf[i] = '\\';
        }
    }
    return CreateDirectoryA(buf, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

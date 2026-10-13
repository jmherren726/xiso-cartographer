#include "file_source.h"

#include <limits.h>

#if defined(_WIN32)
#define SEEK64(f, off, whence) _fseeki64((f), (long long)(off), (whence))
#define TELL64(f) _ftelli64(f)
#else
/* LP64 hosts (Linux, macOS): long is 64-bit, so plain fseek/ftell reach past 4 GiB. */
_Static_assert(sizeof(long) >= 8, "host tests need a 64-bit long");
#define SEEK64(f, off, whence) fseek((f), (long)(off), (whence))
#define TELL64(f) ftell(f)
#endif

static xc_err file_read_at(void *ctx, uint64_t offset, void *buf, size_t len)
{
    test_file_source *fs = ctx;
    if (offset > (uint64_t)LLONG_MAX || SEEK64(fs->f, offset, SEEK_SET) != 0) {
        return XC_ERR_IO;
    }
    return fread(buf, 1, len, fs->f) == len ? XC_OK : XC_ERR_IO;
}

xc_err test_file_source_open(xc_source *src, test_file_source *fs, const char *path)
{
    fs->f = fopen(path, "rb");
    if (fs->f == NULL) {
        return XC_ERR_IO;
    }
    if (SEEK64(fs->f, 0, SEEK_END) != 0) {
        test_file_source_close(fs);
        return XC_ERR_IO;
    }
    long long end = TELL64(fs->f);
    if (end < 0) {
        test_file_source_close(fs);
        return XC_ERR_IO;
    }
    src->read_at = file_read_at;
    src->ctx = fs;
    src->size = (uint64_t)end;
    return XC_OK;
}

void test_file_source_close(test_file_source *fs)
{
    if (fs->f != NULL) {
        (void)fclose(fs->f);
        fs->f = NULL;
    }
}

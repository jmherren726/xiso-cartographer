#include <limits.h>

#include "host_path.h"
#include "xcat/host.h"

#if defined(_WIN32)
#define SEEK64(f, off, whence) _fseeki64((f), (long long)(off), (whence))
#define TELL64(f) _ftelli64(f)
#else
/* LP64 hosts (Linux, macOS): long is 64-bit, so plain fseek/ftell reach past 4 GiB. */
_Static_assert(sizeof(long) >= 8, "host builds need a 64-bit long");
#define SEEK64(f, off, whence) fseek((f), (long)(off), (whence))
#define TELL64(f) ftell(f)
#endif

static xc_err file_read_at(void *ctx, uint64_t offset, void *buf, size_t len)
{
    xc_host_file *hf = ctx;
    if (offset > (uint64_t)LLONG_MAX || SEEK64(hf->f, offset, SEEK_SET) != 0) {
        return XC_ERR_IO;
    }
    return fread(buf, 1, len, hf->f) == len ? XC_OK : XC_ERR_IO;
}

xc_err xc_host_file_open(xc_source *src, xc_host_file *hf, const char *path)
{
    if (src == NULL || hf == NULL || path == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    hf->f = xc_host_fopen(path, "rb");
    if (hf->f == NULL) {
        return XC_ERR_IO;
    }
    if (SEEK64(hf->f, 0, SEEK_END) != 0) {
        xc_host_file_close(hf);
        return XC_ERR_IO;
    }
    long long end = TELL64(hf->f);
    if (end < 0) {
        xc_host_file_close(hf);
        return XC_ERR_IO;
    }
    src->read_at = file_read_at;
    src->ctx = hf;
    src->size = (uint64_t)end;
    return XC_OK;
}

void xc_host_file_close(xc_host_file *hf)
{
    if (hf != NULL && hf->f != NULL) {
        (void)fclose(hf->f);
        hf->f = NULL;
    }
}

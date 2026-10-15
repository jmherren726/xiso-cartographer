#include "xcat/source.h"

#include <string.h>

xc_err xc_source_read(const xc_source *src, uint64_t offset, void *buf, size_t len)
{
    if (src == NULL || src->read_at == NULL || (buf == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    if (offset > src->size || (uint64_t)len > src->size - offset) {
        return XC_ERR_RANGE;
    }
    if (len == 0) {
        return XC_OK;
    }
    return src->read_at(src->ctx, offset, buf, len);
}

static xc_err mem_read_at(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const xc_mem_source *mem = ctx;
    if (offset > mem->len || (uint64_t)len > (uint64_t)mem->len - offset) {
        return XC_ERR_RANGE;
    }
    memcpy(buf, mem->data + (size_t)offset, len);
    return XC_OK;
}

void xc_source_memory(xc_source *src, xc_mem_source *mem, const void *data, size_t len)
{
    mem->data = data;
    mem->len = len;
    src->read_at = mem_read_at;
    src->read_stream = NULL;
    src->ctx = mem;
    src->size = len;
}

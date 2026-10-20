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

static xc_err win_read_at(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const xc_window_source *w = ctx;
    return xc_source_read(w->inner, w->base + offset, buf, len);
}

typedef struct win_stream {
    uint64_t base;
    xc_source_data_fn fn;
    void *fn_ctx;
} win_stream;

static xc_err win_data(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    const win_stream *ws = ctx;
    return ws->fn(ws->fn_ctx, offset - ws->base, data, len);
}

static xc_err win_read_stream(void *ctx, uint64_t offset, uint64_t len, xc_source_data_fn fn,
                              void *fn_ctx)
{
    const xc_window_source *w = ctx;
    win_stream ws = {w->base, fn, fn_ctx};
    return w->inner->read_stream(w->inner->ctx, w->base + offset, len, win_data, &ws);
}

void xc_source_window(xc_source *src, xc_window_source *win, const xc_source *inner, uint64_t base,
                      uint64_t size)
{
    win->inner = inner;
    win->base = base;
    src->read_at = win_read_at;
    src->read_stream = inner->read_stream != NULL ? win_read_stream : NULL;
    src->ctx = win;
    src->size = size;
}

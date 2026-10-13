/*
 * XDVDFS reader. Implements docs/formats/xdvdfs.md; "§n" below refers to it.
 *
 * Memory: one fixed table buffer (MAX_TABLE_READ, allocated up front), a
 * visited map and a stack sized for the largest directory seen, plus the
 * growing output. Nothing recurses.
 */
#include "xcat/xdvdfs.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bytes.h"

#define SECTOR 2048U
#define VD_OFFSET 0x10000U
#define VD_ROOT_SECTOR 0x14U
#define VD_ROOT_SIZE 0x18U
#define VD_FILETIME 0x1CU
#define VD_TAIL_MAGIC 0x7ECU
#define MAGIC_LEN 20U
#define ENTRY_HEADER 14U
#define ATTR_DIRECTORY 0x10U
#define PAD16 0xFFFFU
/* §3 rule 1: u16 child offsets in 4-byte units can't reach past this. */
#define MAX_TABLE_READ (0x40000U + SECTOR)

static const char magic[MAGIC_LEN + 1] = "MICROSOFT*XBOX*MEDIA";

/* §1: probe order. */
static const struct {
    xc_xdvdfs_layout layout;
    uint64_t offset;
} layouts[] = {
    {XC_XDVDFS_XISO, 0},
    {XC_XDVDFS_XGD1, 0x18300000U},
    {XC_XDVDFS_XGD2, 0x0FD90000U},
    {XC_XDVDFS_XGD3, 0x02080000U},
};

void xc_xdvdfs_default_limits(xc_xdvdfs_limits *out)
{
    /* §6 */
    out->max_entries = 65536;
    out->max_depth = 64;
    out->max_path_len = 1024;
}

static uint64_t sector_offset(uint64_t partition, uint32_t sector)
{
    return partition + (uint64_t)sector * SECTOR;
}

static bool extent_ok(const xc_source *src, uint64_t offset, uint64_t len)
{
    return offset <= src->size && len <= src->size - offset;
}

xc_err xc_xdvdfs_open(const xc_source *src, xc_xdvdfs_volume *out)
{
    if (src == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        uint8_t vd[SECTOR];
        uint64_t at = layouts[i].offset + VD_OFFSET;
        if (!extent_ok(src, at, SECTOR)) {
            continue;
        }
        xc_err err = xc_source_read(src, at, vd, sizeof vd);
        if (err != XC_OK) {
            return err;
        }
        if (memcmp(vd, magic, MAGIC_LEN) != 0) {
            continue;
        }
        /* §2: a matching leading magic commits to this layout. */
        if (memcmp(vd + VD_TAIL_MAGIC, magic, MAGIC_LEN) != 0) {
            return XC_ERR_CORRUPT;
        }
        xc_xdvdfs_volume v;
        v.layout = layouts[i].layout;
        v.partition_offset = layouts[i].offset;
        if (!xc_rd_u32le(vd, sizeof vd, VD_ROOT_SECTOR, &v.root_sector) ||
            !xc_rd_u32le(vd, sizeof vd, VD_ROOT_SIZE, &v.root_size) ||
            !xc_rd_u64le(vd, sizeof vd, VD_FILETIME, &v.filetime)) {
            return XC_ERR_CORRUPT;
        }
        bool no_files = v.root_sector == 0 && v.root_size == 0;
        if (!no_files &&
            !extent_ok(src, sector_offset(v.partition_offset, v.root_sector), v.root_size)) {
            return XC_ERR_CORRUPT;
        }
        *out = v;
        return XC_OK;
    }
    return XC_ERR_NOT_XDVDFS;
}

/* --- listing ---------------------------------------------------------------- */

typedef struct item {
    size_t path_off; /* into builder.paths */
    uint64_t offset;
    uint32_t size;
    uint32_t sector;
    uint32_t depth; /* directories: depth of this directory */
    uint8_t attributes;
} item;

typedef struct name_ref {
    const uint8_t *p;
    size_t len;
} name_ref;

typedef struct builder {
    const xc_source *src;
    uint64_t partition;
    xc_xdvdfs_limits lim;
    item *files;
    size_t nfiles, cap_files;
    item *dirs; /* also the queue of directories still to walk */
    size_t ndirs, cap_dirs;
    char *paths;
    size_t paths_len, cap_paths;
    uint8_t *table; /* MAX_TABLE_READ bytes, allocated once */
    uint8_t *seen;  /* one byte per 4-byte slot of the current table */
    size_t cap_seen;
    uint32_t *stack;
    size_t cap_stack;
    name_ref *names; /* names in the current directory, for the duplicate check */
    size_t nnames, cap_names;
    uint64_t total;
} builder;

/*
 * Returns `p` resized to hold at least `need` elements of `elem` bytes, updating
 * *cap; or NULL on overflow or allocation failure, leaving `p` and *cap untouched.
 * Callers assign the result back to their typed pointer.
 */
static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
    if (need == 0) {
        need = 1; /* so a non-NULL result always means "usable" */
    }
    if (need <= *cap) {
        return p;
    }
    size_t n = *cap ? *cap : 16;
    while (n < need) {
        if (n > SIZE_MAX / 2) {
            return NULL;
        }
        n *= 2;
    }
    if (n > SIZE_MAX / elem) {
        return NULL;
    }
    void *q = realloc(p, n * elem);
    if (q != NULL) {
        *cap = n;
    }
    return q;
}

static int fold(uint8_t c)
{
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

static int name_cmp(const void *a, const void *b)
{
    const name_ref *x = a;
    const name_ref *y = b;
    size_t n = x->len < y->len ? x->len : y->len;
    for (size_t i = 0; i < n; i++) {
        int d = fold(x->p[i]) - fold(y->p[i]);
        if (d != 0) {
            return d;
        }
    }
    return (x->len > y->len) - (x->len < y->len);
}

/* §4 */
static bool name_ok(const uint8_t *n, size_t len)
{
    if (len == 0 || (len == 1 && n[0] == '.') || (len == 2 && n[0] == '.' && n[1] == '.')) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (n[i] < 0x20 || n[i] == '/' || n[i] == '\\') {
            return false;
        }
    }
    return true;
}

static xc_err add_path(builder *b, size_t parent_off, size_t parent_len, const uint8_t *name,
                       size_t nlen, size_t *out_off)
{
    size_t len = parent_len ? parent_len + 1 + nlen : nlen;
    if (len > b->lim.max_path_len) {
        return XC_ERR_LIMIT;
    }
    char *paths = grow(b->paths, &b->cap_paths, b->paths_len + len + 1, 1);
    if (paths == NULL) {
        return XC_ERR_NOMEM;
    }
    b->paths = paths;
    char *dst = b->paths + b->paths_len;
    if (parent_len) {
        memcpy(dst, b->paths + parent_off, parent_len);
        dst[parent_len] = '/';
        dst += parent_len + 1;
    }
    memcpy(dst, name, nlen);
    dst[nlen] = '\0';
    *out_off = b->paths_len;
    b->paths_len += len + 1;
    return XC_OK;
}

static xc_err add_entry(builder *b, size_t parent_off, size_t parent_len, uint32_t depth,
                        const uint8_t *name, size_t nlen, uint32_t sector, uint32_t size,
                        uint8_t attr)
{
    if (b->nfiles + b->ndirs >= b->lim.max_entries) {
        return XC_ERR_LIMIT;
    }
    item it;
    it.offset = sector_offset(b->partition, sector);
    it.size = size;
    it.sector = sector;
    it.attributes = attr;
    it.depth = depth + 1;
    bool is_dir = (attr & ATTR_DIRECTORY) != 0;
    if (is_dir) {
        if (it.depth > b->lim.max_depth) {
            return XC_ERR_LIMIT; /* also stops tables that point back to an ancestor */
        }
    } else if (size > 0 && !extent_ok(b->src, it.offset, size)) {
        return XC_ERR_CORRUPT; /* §5: data past the end, e.g. a truncated image */
    }
    xc_err err = add_path(b, parent_off, parent_len, name, nlen, &it.path_off);
    if (err != XC_OK) {
        return err;
    }
    if (is_dir) {
        item *dirs = grow(b->dirs, &b->cap_dirs, b->ndirs + 1, sizeof *dirs);
        if (dirs == NULL) {
            return XC_ERR_NOMEM;
        }
        b->dirs = dirs;
        b->dirs[b->ndirs++] = it;
    } else {
        item *files = grow(b->files, &b->cap_files, b->nfiles + 1, sizeof *files);
        if (files == NULL) {
            return XC_ERR_NOMEM;
        }
        b->files = files;
        b->files[b->nfiles++] = it;
        b->total += size;
    }
    return XC_OK;
}

/* Reads one directory table (§3) and adds its entries; subdirectories are queued in b->dirs. */
static xc_err walk_dir(builder *b, uint32_t sector, uint32_t size, size_t path_off, size_t path_len,
                       uint32_t depth)
{
    if (size == 0) {
        return XC_OK; /* §3 rule 2 */
    }
    uint64_t at = sector_offset(b->partition, sector);
    if (!extent_ok(b->src, at, size)) {
        return XC_ERR_CORRUPT;
    }
    size_t tl = size < MAX_TABLE_READ ? size : MAX_TABLE_READ;
    xc_err err = xc_source_read(b->src, at, b->table, tl);
    if (err != XC_OK) {
        return err;
    }
    const uint8_t *t = b->table;
    uint16_t first = 0;
    if (!xc_rd_u16le(t, tl, 0, &first)) {
        return XC_ERR_CORRUPT;
    }
    if (first == PAD16) {
        return XC_OK; /* §3 rule 2: empty directory */
    }

    size_t slots = tl / 4 + 1;
    uint8_t *seen = grow(b->seen, &b->cap_seen, slots, 1);
    if (seen == NULL) {
        return XC_ERR_NOMEM;
    }
    b->seen = seen;
    uint32_t *stack = grow(b->stack, &b->cap_stack, 2 * slots + 1, sizeof *stack);
    if (stack == NULL) {
        return XC_ERR_NOMEM;
    }
    b->stack = stack;
    memset(b->seen, 0, slots);
    b->nnames = 0;

    /* Each slot is processed at most once and pushes at most two children, so the stack can't
       exceed 2 * slots. */
    size_t sp = 0;
    b->stack[sp++] = 0;
    while (sp > 0) {
        uint32_t slot = b->stack[--sp];
        if (slot >= slots || b->seen[slot]) {
            return XC_ERR_CORRUPT; /* §3 rule 4: reached twice */
        }
        b->seen[slot] = 1;
        size_t pos = (size_t)slot * 4U;
        uint16_t left = 0;
        uint16_t right = 0;
        uint32_t esector = 0;
        uint32_t esize = 0;
        uint8_t attr = 0;
        uint8_t nlen = 0;
        if (!xc_rd_u16le(t, tl, pos, &left) || !xc_rd_u16le(t, tl, pos + 2, &right) ||
            !xc_rd_u32le(t, tl, pos + 4, &esector) || !xc_rd_u32le(t, tl, pos + 8, &esize) ||
            !xc_rd_u8(t, tl, pos + 12, &attr) || !xc_rd_u8(t, tl, pos + 13, &nlen)) {
            return XC_ERR_CORRUPT; /* §3 rule 3 */
        }
        if (left == PAD16 || !xc_in_bounds(tl, pos + ENTRY_HEADER, nlen)) {
            return XC_ERR_CORRUPT; /* §3 rule 3: padding or past the table */
        }
        const uint8_t *name = t + pos + ENTRY_HEADER;
        if (!name_ok(name, nlen)) {
            return XC_ERR_CORRUPT;
        }
        name_ref *names = grow(b->names, &b->cap_names, b->nnames + 1, sizeof *names);
        if (names == NULL) {
            return XC_ERR_NOMEM;
        }
        b->names = names;
        b->names[b->nnames].p = name;
        b->names[b->nnames].len = nlen;
        b->nnames++;

        err = add_entry(b, path_off, path_len, depth, name, nlen, esector, esize, attr);
        if (err != XC_OK) {
            return err;
        }
        if (right != 0) {
            b->stack[sp++] = right;
        }
        if (left != 0) {
            b->stack[sp++] = left;
        }
    }

    /* §4: names equal after case folding would overwrite each other on extraction. */
    qsort(b->names, b->nnames, sizeof *b->names, name_cmp);
    for (size_t i = 1; i < b->nnames; i++) {
        if (name_cmp(&b->names[i - 1], &b->names[i]) == 0) {
            return XC_ERR_CORRUPT;
        }
    }
    return XC_OK;
}

static int file_cmp(const void *a, const void *b)
{
    const xc_xdvdfs_entry *x = a;
    const xc_xdvdfs_entry *y = b;
    if (x->offset != y->offset) {
        return x->offset < y->offset ? -1 : 1;
    }
    return strcmp(x->path, y->path);
}

static xc_err to_entries(const builder *b, const item *items, size_t n, xc_xdvdfs_entry **out)
{
    *out = NULL;
    if (n == 0) {
        return XC_OK;
    }
    if (n > SIZE_MAX / sizeof **out) {
        return XC_ERR_NOMEM;
    }
    xc_xdvdfs_entry *e = malloc(n * sizeof *e);
    if (e == NULL) {
        return XC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        e[i].path = b->paths + items[i].path_off;
        e[i].offset = items[i].offset;
        e[i].size = items[i].size;
        e[i].sector = items[i].sector;
        e[i].attributes = items[i].attributes;
    }
    *out = e;
    return XC_OK;
}

xc_err xc_xdvdfs_list(const xc_source *src, const xc_xdvdfs_volume *vol,
                      const xc_xdvdfs_limits *limits, xc_xdvdfs_listing *out)
{
    if (src == NULL || vol == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);

    builder b;
    memset(&b, 0, sizeof b);
    b.src = src;
    b.partition = vol->partition_offset;
    if (limits != NULL) {
        b.lim = *limits;
    } else {
        xc_xdvdfs_default_limits(&b.lim);
    }

    /* The root's path is empty: give it a terminator so offset 0 is a valid string. */
    b.table = malloc(MAX_TABLE_READ);
    b.paths = grow(NULL, &b.cap_paths, 1, 1);
    xc_err err = (b.table != NULL && b.paths != NULL) ? XC_OK : XC_ERR_NOMEM;
    if (err == XC_OK) {
        b.paths[0] = '\0';
        b.paths_len = 1;
        err = walk_dir(&b, vol->root_sector, vol->root_size, 0, 0, 0);
    }
    /* Breadth-first: b.dirs grows while we walk it, so copy each item before use. */
    for (size_t i = 0; err == XC_OK && i < b.ndirs; i++) {
        item d = b.dirs[i];
        err = walk_dir(&b, d.sector, d.size, d.path_off, strlen(b.paths + d.path_off), d.depth);
    }
    if (err == XC_OK) {
        err = to_entries(&b, b.files, b.nfiles, &out->files);
    }
    if (err == XC_OK) {
        err = to_entries(&b, b.dirs, b.ndirs, &out->dirs);
    }

    free(b.files);
    free(b.dirs);
    free(b.table);
    free(b.seen);
    free(b.stack);
    free(b.names);
    if (err != XC_OK) {
        free(out->files);
        free(out->dirs);
        free(b.paths);
        memset(out, 0, sizeof *out);
        return err;
    }
    out->file_count = b.nfiles;
    out->dir_count = b.ndirs;
    out->total_file_bytes = b.total;
    out->paths_ = b.paths;
    if (out->files != NULL) {
        qsort(out->files, out->file_count, sizeof *out->files, file_cmp);
    }
    return XC_OK;
}

void xc_xdvdfs_listing_free(xc_xdvdfs_listing *listing)
{
    if (listing == NULL) {
        return;
    }
    free(listing->files);
    free(listing->dirs);
    free(listing->paths_);
    memset(listing, 0, sizeof *listing);
}

#ifndef XCAT_NXDK_H
#define XCAT_NXDK_H

/*
 * Xbox (nxdk) platform adapters: a file-backed xc_source, a FATX directory
 * xc_sink, drive helpers, and lwIP networking with a clock (docs/ARCHITECTURE.md §2, §4). Built
 * only into XBEs (app/Makefile, app/net-test/Makefile). Paths are DOS-style ("E:\Games\Title");
 * FATX limits paths to 250 bytes after the drive (docs/formats/fatx-names.md §3), so 256-byte
 * buffers always suffice once xc_fatx_check_listing() has passed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/install.h"
#include "xcat/net.h"
#include "xcat/sink.h"
#include "xcat/source.h"

#define XC_NX_PATH_MAX 256 /* bytes, including the terminator */

typedef struct xc_nx_file {
    void *handle; /* HANDLE */
} xc_nx_file;

/* Opens `path` read-only as a source (64-bit offsets). `f` must outlive `src`. */
xc_err xc_nx_file_open(xc_source *src, xc_nx_file *f, const char *path);
void xc_nx_file_close(xc_nx_file *f);

typedef struct xc_nx_dir_sink {
    char root[XC_NX_PATH_MAX];
    char open_path[XC_NX_PATH_MAX];
    void *handle; /* HANDLE of the open file, or NULL */
    uint32_t expected;
    uint64_t written;
} xc_nx_dir_sink;

/* Writes into `root`, creating it and missing parents. Partial files are deleted. */
xc_err xc_nx_dir_sink_open(xc_sink *sink, xc_nx_dir_sink *ds, const char *root);
void xc_nx_dir_sink_close(xc_nx_dir_sink *ds);

/* Mounts E: (HDD partition 1) if needed. */
bool xc_nx_mount_e(void);

/* Free bytes on the drive containing `path` (e.g. "E:\"). */
xc_err xc_nx_free_space(const char *path, uint64_t *free_bytes);

/* True if `path` names an existing file or directory. */
bool xc_nx_exists(const char *path);

/* Creates `path` and any missing parents. */
bool xc_nx_mkdirs(const char *path);

/*
 * Networking (xcat/net.h) over nxdk's lwIP (nx_net.c). xc_nx_net_startup()
 * brings the network up with the dashboard's settings (DHCP or static) and
 * writes the console's IPv4 address as text into ip[0, ip_size). It fails with
 * XC_ERR_NET_CONNECT if the settings can't be read or DHCP gets no address
 * (cable unplugged?). Call it once, before using `net`.
 */
xc_err xc_nx_net_startup(char *ip, size_t ip_size);
void xc_nx_net(xc_net *net);

/* Monotonic milliseconds from the CPU's performance counter, and sleep. */
void xc_nx_clock(xc_clock *clock);

/* How the last connect went, for diagnosing slow first requests (M3 hardware notes). */
typedef struct xc_nx_net_stats {
    unsigned connects;   /* connect attempts so far */
    uint32_t dns_ms;     /* name lookup of the last one */
    uint32_t connect_ms; /* TCP handshake of the last one */
    xc_err result;       /* XC_OK, or why it failed */
} xc_nx_net_stats;
void xc_nx_net_last(xc_nx_net_stats *out);

/* File access for xc_install (xcat/install.h) on FATX, with flushed, durable writes. */
void xc_nx_install_io(xc_install_io *io);

#endif /* XCAT_NXDK_H */

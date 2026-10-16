#ifndef XCAT_HOST_H
#define XCAT_HOST_H

/*
 * Host (PC) platform adapters for tests and xcat-cli: a file-backed xc_source,
 * a directory xc_sink, and sockets and a clock for the HTTP client. Never built
 * for the Xbox (docs/ARCHITECTURE.md §2).
 *
 * Paths are UTF-8. On Windows they are converted to UTF-16 and given the
 * "\\?\" long-path prefix, so paths over MAX_PATH (e.g. 255-character XDVDFS
 * names) work without the LongPathsEnabled policy.
 */

#include <stdbool.h>
#include <stdio.h>

#include "xcat/install.h"
#include "xcat/net.h"
#include "xcat/sink.h"
#include "xcat/source.h"
#include "xcat/tls.h"

#ifdef __cplusplus
extern "C" {
#endif

/* fopen with a UTF-8 path; long paths work on Windows. */
FILE *xc_host_fopen(const char *path, const char *mode);
/* Deletes a file (UTF-8 path); returns true on success. */
bool xc_host_remove(const char *path);

typedef struct xc_host_file {
    FILE *f;
} xc_host_file;

/* Opens `path` read-only as a source (64-bit offsets). `hf` must outlive `src`. */
xc_err xc_host_file_open(xc_source *src, xc_host_file *hf, const char *path);
void xc_host_file_close(xc_host_file *hf);

typedef struct xc_host_dir_sink {
    char *root;      /* owned */
    char *open_path; /* full path of the open file; owned */
    FILE *f;         /* the open file, or NULL */
    uint32_t expected;
    uint64_t written;
} xc_host_dir_sink;

/*
 * Writes into directory `root`, creating it and any missing parents. Existing
 * files are overwritten; partial files are removed.
 */
xc_err xc_host_dir_sink_open(xc_sink *sink, xc_host_dir_sink *ds, const char *root);
void xc_host_dir_sink_close(xc_host_dir_sink *ds);

/*
 * Networking (xcat/net.h) over the OS sockets API: Winsock on Windows, BSD
 * sockets elsewhere. Call xc_host_net_startup() once before using `net` and
 * xc_host_net_cleanup() at the end. Name lookup uses getaddrinfo(), which can't
 * be cancelled, so the connect timeout starts after it returns.
 */
xc_err xc_host_net_startup(void);
void xc_host_net_cleanup(void);
void xc_host_net(xc_net *net);

/* Monotonic milliseconds and sleep (xcat/net.h). */
void xc_host_clock(xc_clock *clock);

/*
 * For TLS (xcat/tls.h): the OS random generator (BCryptGenRandom, /dev/urandom) and the
 * OS clock. A PC's clock is trusted as it is; the console gets NTP (ADR 0012 §5).
 */
void xc_host_entropy(xc_entropy *e);
void xc_host_wallclock(xc_wallclock *w);

/*
 * File access for xc_install (xcat/install.h): the host separator, durable
 * appends and writes (fsync / _commit), and the directory sink and file source above.
 */
void xc_host_install_io(xc_install_io *io);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_HOST_H */

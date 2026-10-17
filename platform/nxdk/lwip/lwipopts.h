/*
 * lwIP options for our XBEs: nxdk's own lwipopts.h, plus TCP receive tuning for downloads
 * over the internet (docs/testing.md, M5 TLS speed). This folder comes ahead of nxdk's in the
 * include path: for nxdk's network library when the Docker image builds it
 * (docker/nxdk.Dockerfile), and for our sources (app/lwip.mk). Change it, rebuild the image.
 *
 * Why: nxdk receives with a 64 KB window and no window scaling. One connection then moves
 * at most 64 KB per round trip: ~1.1 MB/s to archive.org at ~60 ms, and the console measured
 * 0.3 MB/s there (6.7 MB/s over HTTPS on the LAN, so not TLS). Without SACK, every lost
 * segment also costs the sender a retransmission timeout.
 */
#ifndef XCAT_LWIPOPTS_H
#define XCAT_LWIPOPTS_H

#include_next "lwipopts.h" /* nxdk's: lib/net/nforceif/include/lwipopts.h */

/*
 * Receive window: 240 segments (~336 KB), advertised with window scaling (RFC 7323). One
 * connection moves at most one window per round trip: 120 segments (167 KB) gave a steady
 * 1.7 MB/s from archive.org with the app idle 79% of the time, which is that window at ~95 ms.
 * 240 segments allow ~3.5 MB/s there. The socket's receive mailbox holds SYS_MBOX_SIZE (128,
 * nxdk's sys_arch.h) segments the app hasn't read; past that, lwIP keeps the data as "refused"
 * and hands it over on a later segment or timer tick (up to 250 ms), so nothing is lost when
 * the app falls behind, it only waits.
 */
#undef TCP_WND
#define TCP_WND (240 * TCP_MSS)
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 3 /* the 16-bit window field counts 8-byte units: up to 512 KB */

/* Sending stays as it was (we only send small requests). */
#undef TCP_SND_BUF
#define TCP_SND_BUF (45 * TCP_MSS)

/*
 * Zero-copy receive buffers in the network driver (nvnetdrv_lwip.c): a segment keeps its
 * buffer until the app has read it, so a full window needs one buffer per segment, plus room
 * for out-of-order segments, frames waiting for lwIP's thread and other traffic. When all are
 * taken, the network chip drops what arrives. 2 KB each: 768 KB in all.
 */
#define RX_BUFF_CNT 384

/* Tell the sender which segments arrived after a loss (SACK, RFC 2018), so it resends only
 * what's missing instead of waiting for a timeout. Needs the out-of-order queue (on). */
#define LWIP_TCP_SACK_OUT 1

/* 32-bit statistics counters: 16-bit ones wrap after 94 MB of frames (xc_nx_tcp_counters_get). */
#define LWIP_STATS_LARGE 1

/* Pool statistics: the most driver receive buffers in use at once (xc_nx_tcp_counters_get). */
#undef MEMP_STATS
#define MEMP_STATS 1

#endif /* XCAT_LWIPOPTS_H */

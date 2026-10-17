/*
 * libFuzzer target for the SNTP answer checks (docs/formats/sntp.md §3–§5). Every input is
 * checked twice: against a fixed nonce, and against its own bytes 24–31 as the nonce, so the
 * fuzzer gets past the nonce check (§4.6) and explores the other checks and the timestamps.
 *
 * Checked after each parse; a violation aborts: an accepted answer passed every check of §4
 * (mode, version, leap indicator, stratum, a set transmit time) and the delay correction
 * never moves the time backwards or by more than half the round trip. A refused answer always
 * has a reason, and a kiss code only with stratum 0. Seeds are in core/tests/fuzz-seeds/sntp/.
 * Built like fuzz_http.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/sntp.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static void parse_and_check(const uint8_t *data, size_t size, const uint8_t nonce[8])
{
    xc_sntp_reply r;
    const char *why = NULL;
    char kiss[5] = "x";
    xc_err err = xc_sntp_parse(data, size, nonce, &r, &why, kiss);
    if (err != XC_OK) {
        check(err == XC_ERR_NTP && why != NULL && strlen(kiss) <= 4);
        check(kiss[0] == '\0' || (size >= 48 && data[1] == 0));
        return;
    }
    check(why == NULL && kiss[0] == '\0');
    check(size >= 48 && (data[0] & 7U) == 4U && (data[0] >> 6U) != 3U);
    check(r.version == 3 || r.version == 4);
    check(r.stratum >= 1 && r.stratum <= 15 && r.stratum == data[1]);
    check(r.transmit_ms != 0 && memcmp(data + 24, nonce, 8) == 0);
    static const uint32_t rtts[] = {0, 1, 57, 2000, 0xFFFFFFFFU};
    for (size_t i = 0; i < sizeof rtts / sizeof rtts[0]; i++) {
        int64_t at = xc_sntp_arrival_ms(&r, rtts[i]);
        check(at >= r.transmit_ms && at - r.transmit_ms <= (int64_t)rtts[i] / 2);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint8_t fixed[8] = {0x5A, 0x01, 0xC3, 0x7E, 0x00, 0xFF, 0x10, 0x99};
    parse_and_check(data, size, fixed);
    if (size >= 32) {
        uint8_t own[8];
        memcpy(own, data + 24, 8);
        parse_and_check(data, size, own);
    }
    return 0;
}

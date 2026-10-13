/*
 * Runs a libFuzzer target over input files without libFuzzer, so every
 * compiler can build the harness and CTest can replay seeds and past crashes.
 *
 * Usage: <target>_replay FILE...
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Inputs are capped well above anything the fuzzer generates. */
#define MAX_INPUT ((size_t)16 * 1024 * 1024)

static int replay(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        (void)fprintf(stderr, "%s: cannot open\n", path);
        return 1;
    }
    uint8_t *buf = malloc(MAX_INPUT);
    if (buf == NULL) {
        (void)fclose(f);
        return 1;
    }
    size_t n = fread(buf, 1, MAX_INPUT, f);
    int status = ferror(f) ? 1 : 0;
    if (fclose(f) != 0) {
        status = 1;
    }
    if (status == 0) {
        (void)LLVMFuzzerTestOneInput(buf, n);
        (void)printf("%s: ok (%zu bytes)\n", path, n);
    } else {
        (void)fprintf(stderr, "%s: read error\n", path);
    }
    free(buf);
    return status;
}

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (replay(argv[i]) != 0) {
            status = 1;
        }
    }
    return status;
}

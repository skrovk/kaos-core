#define _POSIX_C_SOURCE 200809L
#include "kaos_artifact.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { REPEATS = 100 };
static uint64_t timestamp(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) exit(2);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
static int compare(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}
int main(void)
{
    static const size_t sizes[] = {256, 4096, 16384, 65536};
    uint8_t bytes[65536];
    uint64_t elapsed[REPEATS];
    volatile uint32_t observed = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
    puts("{\"clock\":\"CLOCK_MONOTONIC nanoseconds\",\"scope\":\"CRC32 call only; no registry lock or network load\",\"samples\":[");
    for (size_t sample = 0; sample <= sizeof(sizes) / sizeof(*sizes); ++sample) {
        const size_t size = sample == 0 ? 0 : sizes[sample - 1];
        uint32_t checksum = 0;
        for (unsigned repeat = 0; repeat < REPEATS; ++repeat) {
            const uint64_t begin = timestamp();
            if (size) checksum = core_artifact_crc32(bytes, size);
            elapsed[repeat] = timestamp() - begin;
            observed ^= checksum;
        }
        qsort(elapsed, REPEATS, sizeof(*elapsed), compare);
        printf("%s{\"bytes\":%zu,\"repeats\":%u,\"p50_ns\":%" PRIu64
               ",\"p95_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64 ",\"crc32\":%" PRIu32 "}",
               sample ? ",\n" : "", size, REPEATS, elapsed[49], elapsed[94], elapsed[99], checksum);
    }
    printf("],\"observation_sink\":%" PRIu32 "}\n", observed);
    return 0;
}

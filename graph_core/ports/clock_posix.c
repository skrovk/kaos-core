#define _POSIX_C_SOURCE 200809L
#include "kaos_graph.h"

#include <time.h>

bool go_clock_now(uint64_t *out_us)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) {
        return false;
    }
    const uint64_t fraction = (uint64_t)now.tv_nsec / 1000;
    if ((uint64_t)now.tv_sec > (UINT64_MAX - fraction) / 1000000) {
        return false;
    }
    *out_us = (uint64_t)now.tv_sec * 1000000 + fraction;
    return true;
}

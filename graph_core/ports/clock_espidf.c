#include "kaos_graph.h"

#include "esp_timer.h"

bool go_clock_now(uint64_t *out_us)
{
    const int64_t now = esp_timer_get_time();
    if (now < 0) {
        return false;
    }
    *out_us = (uint64_t)now;
    return true;
}

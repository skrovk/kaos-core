#include "workload.h"

static unsigned role;
static const u64 channel[3] = {10, 20, 7};

int kaos_configure(const unsigned char *bytes, unsigned size)
{
    if (size != 1 || bytes[0] > 3) return -1;
    role = bytes[0]; /* finite producer, consumer, producer, finite consumer */
    return 0;
}

void kaos_main(void)
{
    if (init() != 0) return;
    unsigned count = 0;
    sample s;
    for (;;) {
        if (role % 2 == 0 && (role == 2 || count < 16)) {
            fill(&s, count);
            if (send(channel, &s) == 0) ++count;
        } else if (role % 2 == 1 && (role == 1 || count < 8)) {
            if (receive(channel, &s, 0)) ++count;
        }
        /* A stalled application remains alive and cancellable. */
        if (wait_next() < 0) return;
    }
}

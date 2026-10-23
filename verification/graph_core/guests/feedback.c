#include "workload.h"

static unsigned responder;
static const u64 request[3] = {10, 20, 1}, reply[3] = {20, 10, 1};

int kaos_configure(const unsigned char *bytes, unsigned size)
{
    if (size != 1 || bytes[0] > 1) return -1;
    responder = bytes[0];
    return 0;
}
void kaos_main(void)
{
    if (init() != 0) return;
    sample s;
    unsigned sequence = 0, pending = 0;
    for (;;) {
        if (responder) {
            if (!pending && receive(request, &s, 0)) {
                s.value = 2u * s.value + 1u;
                pending = 1;
            }
            if (pending && send(reply, &s) != -2) pending = 0;
        } else {
            if (receive(reply, &s, 1)) {
                if (s.sequence > sequence) __builtin_trap();
                if (s.sequence == sequence) ++sequence;
                /* Older duplicate responses cannot advance the loop. */
            }
            fill(&s, sequence);
            /* Idempotent fixture requests may be retried. This is not a
             * delivery or exactly-once-effects promise for other workloads. */
            (void)send(request, &s);
        }
        if (wait_next() < 0) return;
    }
}

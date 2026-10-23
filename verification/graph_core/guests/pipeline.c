#include "workload.h"

static unsigned role;
static const u64 input[3] = {10, 20, 1}, output[3] = {20, 30, 1};

int kaos_configure(const unsigned char *bytes, unsigned size)
{
    if (size != 1 || bytes[0] > 2) return -1;
    role = bytes[0]; /* source, transform, sink */
    return 0;
}
void kaos_main(void)
{
    if (init() != 0) return;
    sample s;
    unsigned sequence = 0, pending = 0;
    for (;;) {
        if (role == 0) {
            fill(&s, sequence);
            if (send(input, &s) == 0) ++sequence;
        } else if (role == 1) {
            if (!pending && receive(input, &s, 0)) {
                s.value = 2u * s.value + 1u;
                pending = 1;
            }
            /* Hold across FULL, discard across DISABLED; never deliberately
             * carry a pending sample into a replacement output binding. */
            if (pending && send(output, &s) != -2) pending = 0;
        } else {
            (void)receive(output, &s, 1);
        }
        if (wait_next() < 0) return;
    }
}

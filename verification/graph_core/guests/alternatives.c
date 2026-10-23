#include "workload.h"

static unsigned role;
static const u64 channels[2][3] = {{10, 20, 1}, {10, 30, 1}};

int kaos_configure(const unsigned char *bytes, unsigned size)
{
    if (size != 1 || bytes[0] > 2) return -1;
    role = bytes[0]; /* source, sink A, sink B */
    return 0;
}

void kaos_main(void)
{
    if (init() != 0) return;
    unsigned sequences[2] = {0, 0};
    sample s;
    for (;;) {
        if (role == 0) {
            /* Each branch advances independently. DISABLED or FULL on one
             * channel must not prevent trying the other channel. */
            for (unsigned i = 0; i < 2; ++i) {
                fill(&s, sequences[i]);
                if (send(channels[i], &s) == 0) ++sequences[i];
            }
        } else {
            (void)receive(channels[role - 1], &s, 0);
        }
        if (wait_next() < 0) return;
    }
}

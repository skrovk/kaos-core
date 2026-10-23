#include "workload.h"

static unsigned role;
static const u64 inputs[2][3] = {{10, 20, 1}, {11, 20, 1}};
static const u64 output[3] = {20, 30, 1};

int kaos_configure(const unsigned char *bytes, unsigned size)
{
    if (size != 1 || bytes[0] > 3) return -1;
    role = bytes[0]; /* input A, input B, transform, sink */
    return 0;
}
void kaos_main(void)
{
    if (init() != 0) return;
    sample s;
    unsigned sequence = role, pending = 0;
    for (;;) {
        if (role < 2) {
            /* Even/odd identities keep the two source histories distinct. */
            fill(&s, sequence);
            if (send(inputs[role], &s) == 0) sequence += 2;
        } else if (role == 2) {
            if (!pending) {
                for (unsigned i = 0; i < 2; ++i) {
                    if (receive(inputs[i], &s, 0)) {
                        s.value = 2u * s.value + 1u;
                        pending = 1;
                        break;
                    }
                }
            }
            /* Hold one sample across FULL; discard across DISABLED. */
            if (pending && send(output, &s) != -2) pending = 0;
        } else {
            (void)receive(output, &s, 1);
        }
        if (wait_next() < 0) return;
    }
}

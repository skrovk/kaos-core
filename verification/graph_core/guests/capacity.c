#include <stdint.h>

__attribute__((import_module("kaos"), import_name("init"))) int kaos_init(void);
__attribute__((import_module("kaos"), import_name("error"))) int kaos_error(void);
__attribute__((import_module("kaos"), import_name("put")))
int kaos_put(const uint64_t *, const void *, unsigned);
__attribute__((import_module("kaos"), import_name("get")))
int kaos_get(const uint64_t *, void *, unsigned);
__attribute__((import_module("kaos"), import_name("event_get"))) int kaos_event_get(void *, unsigned);
__attribute__((import_module("kaos"), import_name("event_wait"))) int kaos_event_wait(int);

static uint64_t outgoing[4][3], incoming[4][3];
static uint32_t sent[64], received[64], event[8];
static unsigned counter, stop_events;

int kaos_configure(const unsigned char *bytes, unsigned size)
{
    if (size != 24) return -1;
    uint64_t config[3] = {0};
    for (unsigned i = 0; i < 24; ++i) config[i / 8] |= (uint64_t)bytes[i] << (8 * (i % 8));
    for (unsigned c = 0; c < 4; ++c) {
        outgoing[c][0] = incoming[c][1] = config[0];
        outgoing[c][1] = config[2];
        incoming[c][0] = config[1];
        outgoing[c][2] = incoming[c][2] = 7 + c;
    }
    return 0;
}

void kaos_main(void)
{
    kaos_init();
    for (;;) {
        for (unsigned c = 0; c < 4; ++c) {
            for (unsigned i = 0; i < 64; ++i) sent[i] = counter + i;
            sent[0] = (uint32_t)outgoing[c][0];
            sent[1] = ++counter;
            sent[2] = c;
            (void)kaos_put(outgoing[c], sent, sizeof(sent));
            const int size = kaos_get(incoming[c], received, sizeof(received));
            if (size >= 0) {
                if (size != sizeof(received) || received[0] != incoming[c][0] || received[2] != c)
                    __builtin_trap();
                for (unsigned i = 3; i < 64; ++i)
                    if (received[i] != received[1] - 1 + i) __builtin_trap();
                /* Reporting-only ERROR is a bounded, host-attributed witness
                 * that the real guest validated a complete received message. */
                (void)kaos_error();
            }
        }
        if (!stop_events && kaos_event_get(event, sizeof(event)) == 0 && event[6] == 0)
            stop_events = 1; /* leave later removal notifications unread */
        (void)kaos_event_wait(1000);
    }
}

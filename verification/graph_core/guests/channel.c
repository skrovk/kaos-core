/* P4 copied channel ABI fixture; no libc or retained host pointers. */
typedef unsigned long long u64;
__attribute__((import_module("kaos"), import_name("init")))
int kaos_init(void);
__attribute__((import_module("kaos"), import_name("error")))
int kaos_error(void);
__attribute__((import_module("kaos"), import_name("put")))
int kaos_put(const u64 *, const void *, unsigned);
__attribute__((import_module("kaos"), import_name("get")))
int kaos_get(const u64 *, void *, unsigned);
__attribute__((import_module("kaos"), import_name("event_get")))
int kaos_event_get(void *, unsigned);
__attribute__((import_module("kaos"), import_name("event_wait")))
int kaos_event_wait(int);

static const u64 channel[] = {10, 20, 7};
static const u64 unknown[] = {10, 20, 8};
static unsigned char payload[8] = {'a', 'b'};
static struct { u64 channel[3]; unsigned permissions, reserved; } event;

static void check(int condition) { if (!condition) __builtin_trap(); }

int kaos_configure(const unsigned char *configuration, unsigned size)
{
    (void)configuration;
    (void)size;
    check(kaos_put(channel, payload, 2) == -1);
    check(kaos_get(channel, payload, 2) == -1);
    check(kaos_event_get(&event, sizeof(event)) == -1);
    return 0;
}

void kaos_main(void)
{
    check(kaos_init() == 0);
    check(kaos_put((const u64 *)0xffffffffu, payload, 1) == -4);
    check(kaos_put(channel, (const void *)0xffffffffu, 1) == -4);
    check(kaos_put(channel, payload, 0xffffffffu) == -4);
    check(kaos_get((const u64 *)0xffffffffu, payload, 1) == -4);
    check(kaos_get(channel, (void *)0xffffffffu, 1) == -4);
    check(kaos_get(channel, payload, 0xffffffffu) == -4);
    check(kaos_event_get((void *)0xffffffffu, 32) == -4);
    check(kaos_put(channel, payload, 2) == -1);
    check(kaos_get(channel, payload, 2) == -1);
    check(kaos_event_get(&event, sizeof(event)) == -3);
    check(kaos_event_wait(-1) == -1);
    check(kaos_event_wait(1000001) == -1);
    check(kaos_event_wait(0) == 1);
    check(kaos_event_wait(1000) == 1);
    check(kaos_error() == 0); /* Host publishes permission during the wait gap. */
    check(kaos_event_wait(1000000) == 0);
    check(kaos_event_wait(0) == 0); /* Waiting does not consume the event. */
    check(kaos_event_get(&event, 31) == -5);
    check(kaos_event_get((void *)0xffffffffu, 32) == -4);
    check(kaos_event_get(&event, sizeof(event)) == 0);
    check(event.channel[0] == 10 && event.channel[1] == 20 && event.channel[2] == 7);
    check(event.permissions == 3 && event.reserved == 0);
    check(kaos_event_get(&event, sizeof(event)) == -3);
    check(kaos_put(unknown, payload, 2) == -1);
    check(kaos_put(channel, payload, 5) == -5);
    check(kaos_put(channel, payload, 2) == 0);
    payload[0] = 'c'; payload[1] = 'd';
    check(kaos_put(channel, payload, 2) == 0);
    payload[0] = 'z';
    check(kaos_put(channel, payload, 2) == -2);
    check(kaos_get(channel, payload, 1) == -5);
    check(kaos_get(channel, (void *)0xffffffffu, 2) == -4);
    check(kaos_get(channel, payload, 2) == 2);
    check(payload[0] == 'a' && payload[1] == 'b');
    check(kaos_get(channel, payload, 2) == 2);
    check(payload[0] == 'c' && payload[1] == 'd');
    check(kaos_put(channel, (const void *)0, 0) == 0);
    check(kaos_get(channel, (void *)0, 0) == 0);
    check(kaos_get(channel, payload, 2) == -3);
    check(kaos_error() == 0); /* Cancellation must interrupt the next wait. */
    for (;;) (void)kaos_event_wait(1000000);
}

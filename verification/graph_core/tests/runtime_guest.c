/* Restricted guest: mode arrives through the explicit configuration call.
 * No libc, start function, constructor or guest threads. */
__attribute__((import_module("kaos"), import_name("init")))
int kaos_init(void);
__attribute__((import_module("kaos"), import_name("error")))
int kaos_error(void);
__attribute__((import_module("kaos"), import_name("event_wait")))
int kaos_wait(int timeout_us);

static unsigned mode;

int kaos_configure(const unsigned char *configuration, unsigned size)
{
    mode = size ? configuration[0] : 0;
    if (mode == 1) {
        if (kaos_error() != 0) __builtin_trap();
        for (;;) __asm__ volatile("");
    }
    if (mode == 2) return 1;
    if (mode == 3) __builtin_trap();
    if (mode == 9 && kaos_init() != -1) __builtin_trap();
    if (mode == 10) {
        if (kaos_error() != 0) __builtin_trap();
        for (;;) (void)kaos_wait(1000000);
    }
    return 0;
}

void kaos_main(void)
{
    if (mode == 5) return;
    if (kaos_init() != 0) __builtin_trap();
    if (kaos_init() != -1) __builtin_trap();
    if (mode == 4) for (;;) __asm__ volatile("");
    if (mode == 6) return;
    if (mode == 7) __builtin_trap();
    if (mode == 8) {
        for (unsigned i = 0; i < 4; ++i) if (kaos_error() != 0) __builtin_trap();
        if (kaos_error() != -2) __builtin_trap();
    }
    for (;;) (void)kaos_wait(1000000);
}

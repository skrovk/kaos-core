/* Restricted P3 guest. No libc, constructors, implicit start or guest threads.
 * Configuration chooses a lifecycle scenario; each main entry starts with
 * INIT except the explicit missing-marker/fault fixtures. */
__attribute__((import_module("kaos"), import_name("init")))
extern int init(void);
__attribute__((import_module("kaos"), import_name("error")))
extern int error(void);
__attribute__((import_module("kaos"), import_name("event_wait")))
extern int event_wait(int timeout_us);

static unsigned mode;

int kaos_configure(const unsigned char *bytes, unsigned size)
{
    mode = size ? bytes[0] : 0;
    if (mode == 1) return -1;
    if (mode == 2) for (;;) __asm__ volatile("");
    if (mode == 9 && init() != -1) return -1;
    if (mode == 10) while (event_wait(1000000) >= 0) {}
    return 0;
}

void kaos_main(void)
{
    if (mode == 3) for (;;) __asm__ volatile("");
    if (mode == 5) return;
    if (mode == 7) __builtin_trap();
    if (init() != 0) return;
    if (mode == 4) for (;;) __asm__ volatile("");
    if (mode == 6) return;
    if (mode == 8) { (void)event_wait(200000); return; }
    if (mode == 11) for (unsigned i = 0; i < 8; ++i) (void)error();
    while (event_wait(1000000) >= 0) {}
}

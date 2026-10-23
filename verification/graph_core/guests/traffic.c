/* P4 real-process traffic fixture. Diagnostics mark first reception and the
 * guest's own observation of removal; neither marker changes graph outcomes. */
typedef unsigned long long u64;
__attribute__((import_module("kaos"), import_name("init")))
extern int init(void);
__attribute__((import_module("kaos"), import_name("error")))
extern int error(void);
__attribute__((import_module("kaos"), import_name("put")))
extern int put(const u64 *, const void *, unsigned);
__attribute__((import_module("kaos"), import_name("get")))
extern int get(const u64 *, void *, unsigned);
__attribute__((import_module("kaos"), import_name("event_get")))
extern int event_get(void *, unsigned);
__attribute__((import_module("kaos"), import_name("event_wait")))
extern int event_wait(int);

static unsigned receiver;
static const u64 channel[3] = {10, 20, 7};
static const unsigned char message[4] = {'k', 'a', 'o', 's'};

int kaos_configure(const unsigned char *bytes, unsigned size)
{
    if (size != 1 || bytes[0] > 1) return -1;
    receiver = bytes[0];
    return 0;
}

void kaos_main(void)
{
    if (init() != 0) return;
    unsigned received = 0, enabled = 0, removed = 0, removal_reported = 0;
    for (;;) {
        struct { u64 channel[3]; unsigned permissions, reserved; } event;
        while (event_get(&event, sizeof(event)) == 0) {
            if (event.channel[0] != 10 || event.channel[1] != 20 || event.channel[2] != 7)
                __builtin_trap();
            if (event.permissions != 0) enabled = 1;
            else if (enabled) removed = 1;
        }
        if (removed && !removal_reported && error() == 0) removal_reported = 1;
        if (receiver) {
            unsigned char bytes[4];
            const int result = get(channel, bytes, sizeof(bytes));
            if (result >= 0) {
                if (result != 4 || bytes[0] != 'k' || bytes[1] != 'a' ||
                    bytes[2] != 'o' || bytes[3] != 's') __builtin_trap();
                if (!received && error() == 0) received = 1;
            } else if (result != -1 && result != -3) __builtin_trap();
        } else {
            for (unsigned i = 0; i < 8; ++i) {
                const int result = put(channel, message, sizeof(message));
                if (result != 0 && result != -1 && result != -2) __builtin_trap();
            }
        }
        if (event_wait(1000) < 0) return;
    }
}

/* Real Wasm/native ABI qualification, including recoverable bad arguments. */
#ifndef NATIVE_MODULE
#define NATIVE_MODULE "kaos_native"
#endif
#ifndef NATIVE_NAME
#define NATIVE_NAME "fixture_temperature_read"
#endif
#ifdef NATIVE_BAD_SIGNATURE
typedef long long native_result;
#else
typedef int native_result;
#endif

__attribute__((import_module(NATIVE_MODULE), import_name(NATIVE_NAME)))
extern native_result temperature_read(unsigned sensor, unsigned output,
                                      unsigned capacity);
__attribute__((import_module("kaos"), import_name("init")))
extern int init(void);
__attribute__((import_module("kaos"), import_name("event_wait")))
extern int event_wait(int timeout_us);

static unsigned mode;

int kaos_configure(const unsigned char *bytes, unsigned size)
{
    if (size != 1 || bytes[0] > 1) return -1;
    mode = bytes[0];
    return 0;
}

void kaos_main(void)
{
    unsigned value = 0;
    if (mode == 0) {
        if (temperature_read(0, (unsigned)&value, 4) != 4 || value != 23000 ||
            temperature_read(2, (unsigned)&value, 4) != -4 ||
            temperature_read(0, (unsigned)&value, 3) != -4 ||
            temperature_read(0, (unsigned)&value, 0) != -4 ||
            temperature_read(0, 0xfffffffeu, 4) != -4 ||
            temperature_read(0, 0xffffffffu, 4) != -4)
            __builtin_trap();
    }
    if (init() != 0) return;
    for (;;) {
        if (mode == 1) {
            const native_result result = temperature_read(1, (unsigned)&value, 4);
            if (result == -1) return;
            if (result != 4 || value != 24000) __builtin_trap();
        } else if (event_wait(1000000) < 0) {
            return;
        }
    }
}

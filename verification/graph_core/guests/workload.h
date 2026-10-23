/* Restricted-ABI deterministic fixtures, not an application timing policy. */
typedef unsigned long long u64;
__attribute__((import_module("kaos"), import_name("init")))
extern int init(void);
__attribute__((import_module("kaos"), import_name("put")))
extern int put(const u64 *, const void *, unsigned);
__attribute__((import_module("kaos"), import_name("get")))
extern int get(const u64 *, void *, unsigned);
__attribute__((import_module("kaos"), import_name("event_get")))
extern int event_get(void *, unsigned);
__attribute__((import_module("kaos"), import_name("event_wait")))
extern int event_wait(int);

typedef struct { unsigned sequence, value; unsigned char padding[56]; } sample;
_Static_assert(sizeof(sample) == 64, "fixture wire size");

static void fill(sample *s, unsigned sequence)
{
    s->sequence = s->value = sequence;
    for (unsigned i = 0; i < sizeof(s->padding); ++i)
        s->padding[i] = (unsigned char)(sequence + i);
}
static void check(const sample *s, unsigned transformed)
{
    const unsigned expected = transformed ? 2u * s->sequence + 1u : s->sequence;
    if (s->value != expected) __builtin_trap();
    for (unsigned i = 0; i < sizeof(s->padding); ++i)
        if (s->padding[i] != (unsigned char)(s->sequence + i)) __builtin_trap();
}
static int receive(const u64 channel[3], sample *s, unsigned transformed)
{
    const int result = get(channel, s, sizeof(*s));
    if (result == -1 || result == -3) return 0; /* DISABLED / EMPTY */
    if (result != sizeof(*s)) __builtin_trap();
    check(s, transformed);
    return 1;
}
static int send(const u64 channel[3], const sample *s)
{
    const int result = put(channel, s, sizeof(*s));
    if (result != 0 && result != -1 && result != -2) __builtin_trap();
    return result;
}
static int wait_next(void)
{
    struct { u64 channel[3]; unsigned permissions, reserved; } event;
    while (event_get(&event, sizeof(event)) == 0) {}
    /* Wakeups can return early. This is cooperative pacing, not a periodic
     * acquisition schedule or a guest-visible simulator clock. */
    return event_wait(1000);
}

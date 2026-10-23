#define _POSIX_C_SOURCE 200809L

#include "kaos_wamr.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static core_node_runtime runtime;
static uint8_t *artifact;
static size_t artifact_size;
static pthread_mutex_t changed_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned generation;
static unsigned guard_attempts;
static _Thread_local bool inside_guard;

typedef struct {
    pthread_mutex_t registry;
    unsigned calls, unlocks, deny_call;
    bool allowed, expect_expired;
} guard_state;

static guard_state guard = {.registry = PTHREAD_MUTEX_INITIALIZER};

static core_runtime_state snapshot(void)
{
    core_runtime_state state = *runtime.lock(runtime.context, 0);
    runtime.unlock(runtime.context, 0);
    return state;
}

static void notify(void *unused)
{
    (void)unused;
    /* Publishing execution, guest INIT/ERROR and exit all notify the control
     * task. The registry lock must be released before any of these callbacks. */
    assert(!inside_guard);
    assert(pthread_mutex_lock(&changed_mutex) == 0);
    ++generation;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&changed_mutex) == 0);
}

static bool entry_lock(void *context, size_t slot)
{
    guard_state *g = context;
    assert(g == &guard && slot == 0 && !inside_guard);
    assert(pthread_mutex_lock(&changed_mutex) == 0);
    ++guard_attempts;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&changed_mutex) == 0);
    assert(pthread_mutex_lock(&g->registry) == 0);
    inside_guard = true;
    ++g->calls;
    assert(g->calls <= 2);
    /* Taking the mailbox here also checks that WAMR invoked this callback
     * without holding its mailbox lock: registry precedes mailbox. */
    const core_runtime_state state = snapshot();
    assert(state.prepared && !state.configuration_active && !state.main_active);
    g->allowed = g->calls != g->deny_call;
    return g->allowed; /* retain the registry lock even when denying entry */
}

static void entry_unlock(void *context)
{
    guard_state *g = context;
    assert(g == &guard && inside_guard);
    const core_runtime_state state = snapshot();
    if (g->allowed && !g->expect_expired) {
        assert(g->calls == 1 ? state.configuration_active : state.main_active);
    } else {
        assert(!state.configuration_active && !state.main_active);
    }
    ++g->unlocks;
    inside_guard = false;
    assert(pthread_mutex_unlock(&g->registry) == 0);
}

enum observed { PREPARED, INIT, EXITED, DISPOSED };

static core_runtime_state await(enum observed observation)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    for (;;) {
        assert(pthread_mutex_lock(&changed_mutex) == 0);
        const unsigned before = generation;
        assert(pthread_mutex_unlock(&changed_mutex) == 0);
        core_runtime_state state = snapshot();
        const bool ready = observation == PREPARED ? state.prepared :
            observation == INIT ? state.init_pending :
            observation == EXITED ? state.exited : state.disposed;
        if (ready) return state;
        assert(!state.prepare_failed);
        assert(pthread_mutex_lock(&changed_mutex) == 0);
        int result = 0;
        if (generation == before)
            result = pthread_cond_timedwait(&changed, &changed_mutex, &deadline);
        assert(pthread_mutex_unlock(&changed_mutex) == 0);
        if (result != 0) {
            fprintf(stderr, "entry guard timed out: observation=%d prepared=%d exited=%d init=%d\n",
                    observation, state.prepared, state.exited, state.init_pending);
            abort();
        }
    }
}

static void prepare(unsigned deny_call, uint8_t mode)
{
    assert(pthread_mutex_lock(&guard.registry) == 0);
    guard.calls = guard.unlocks = 0;
    guard.deny_call = deny_call;
    guard.allowed = false;
    guard.expect_expired = false;
    assert(pthread_mutex_unlock(&guard.registry) == 0);
    uint8_t *copy = malloc(artifact_size);
    assert(copy != NULL);
    memcpy(copy, artifact, artifact_size);
    assert(runtime.prepare(runtime.context, 0, copy, artifact_size, &mode, 1,
                           CORE_WAMR_ARENA_BYTES));
    const core_runtime_state state = await(PREPARED);
    assert(!state.init_pending && !state.main_active && !state.configuration_active);
    assert(pthread_mutex_lock(&guard.registry) == 0);
    assert(guard.calls == 0 && guard.unlocks == 0); /* instantiation remains inert */
    assert(pthread_mutex_unlock(&guard.registry) == 0);
}

static void start_before(uint64_t deadline)
{
    core_runtime_state *state = runtime.lock(runtime.context, 0);
    state->start_deadline_us = deadline;
    state->start_requested = true;
    runtime.unlock(runtime.context, 0);
}

static void start(void)
{
    start_before(UINT64_MAX);
}

static void check_unlocked(unsigned calls)
{
    assert(pthread_mutex_trylock(&guard.registry) == 0);
    assert(guard.calls == calls && guard.unlocks == calls);
    assert(pthread_mutex_unlock(&guard.registry) == 0);
}

static void dispose(void)
{
    core_runtime_state *state = runtime.lock(runtime.context, 0);
    state->cancel_requested = true;
    state->dispose_requested = true;
    runtime.unlock(runtime.context, 0);
    assert(await(DISPOSED).disposed);
}

static void registry_wait_cannot_extend_start_deadline(void)
{
    prepare(0, 1);
    struct timespec expiry, wait_limit;
    assert(clock_gettime(CLOCK_MONOTONIC, &expiry) == 0);
    expiry.tv_nsec += 50000000; /* 50 ms, with a deterministic worker barrier */
    if (expiry.tv_nsec >= 1000000000) {
        ++expiry.tv_sec;
        expiry.tv_nsec -= 1000000000;
    }
    const uint64_t deadline = (uint64_t)expiry.tv_sec * 1000000 +
                              (uint64_t)expiry.tv_nsec / 1000;
    assert(pthread_mutex_lock(&changed_mutex) == 0);
    const unsigned before = guard_attempts;
    assert(pthread_mutex_unlock(&changed_mutex) == 0);
    assert(pthread_mutex_lock(&guard.registry) == 0);
    guard.expect_expired = true;
    start_before(deadline);
    assert(clock_gettime(CLOCK_REALTIME, &wait_limit) == 0);
    wait_limit.tv_sec += 5;
    assert(pthread_mutex_lock(&changed_mutex) == 0);
    while (guard_attempts == before)
        assert(pthread_cond_timedwait(&changed, &changed_mutex, &wait_limit) == 0);
    assert(pthread_mutex_unlock(&changed_mutex) == 0);
    /* The worker has entered its guard callback and is waiting for registry.
     * Expire authorization while it waits, then return true from the guard. */
    int waited;
    do {
        waited = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &expiry, NULL);
    } while (waited == EINTR);
    assert(waited == 0);
    assert(pthread_mutex_unlock(&guard.registry) == 0);
    const core_runtime_state state = await(EXITED);
    assert(!state.init_pending && !state.configuration_active && !state.main_active);
    assert(state.errors_pending == 0);
    check_unlocked(1);
    dispose();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file != NULL && fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    artifact_size = (size_t)length;
    artifact = malloc(artifact_size);
    assert(artifact != NULL && fread(artifact, 1, artifact_size, file) == artifact_size);
    assert(fclose(file) == 0);
    assert(core_wamr_init(&runtime, notify, NULL));
    assert(!core_wamr_attach_entry_guard(&guard, NULL, entry_unlock));
    assert(core_wamr_attach_entry_guard(&guard, entry_lock, entry_unlock));
    assert(!core_wamr_attach_entry_guard(&guard, entry_lock, entry_unlock));

    /* Mode 1 emits ERROR and then loops in configuration if entered. */
    prepare(1, 1);
    start();
    core_runtime_state state = await(EXITED);
    assert(!state.init_pending && !state.configuration_active && !state.main_active);
    assert(state.errors_pending == 0);
    check_unlocked(1);
    dispose();

    /* Configuration is allowed, but permission disappears at main entry.
     * Mode 8 would emit INIT and ERROR if even its first instructions ran. */
    prepare(2, 8);
    start();
    state = await(EXITED);
    assert(!state.init_pending && !state.main_active && state.errors_pending == 0);
    check_unlocked(2);
    dispose();

    registry_wait_cannot_extend_start_deadline();

    /* With both guards allowed, a compute-only guest runs while the registry
     * stays available to control, then cancellation and disposal still work. */
    prepare(0, 4);
    start();
    state = await(INIT);
    assert(state.main_active && !state.exited);
    check_unlocked(2);
    dispose();
    check_unlocked(2);
    core_wamr_shutdown();
    free(artifact);
    puts("WAMR entry: live permission checked before configuration/main, registry released on every path");
    return 0;
}

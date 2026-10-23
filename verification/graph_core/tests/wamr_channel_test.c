#define _POSIX_C_SOURCE 200809L
#include "kaos_wamr.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The callback boundary uses a tiny deterministic service double. Endpoint
 * integration separately tests admission, exact bindings and transport. Here
 * the real interpreter exercises untrusted offsets, copied buffers, return
 * codes and the otherwise difficult poll-to-wait wakeup interleaving. */
static core_node_runtime runtime;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool enabled, pending, pause_poll, poll_entered, poll_released;
static uint8_t queue[2][4];
static size_t sizes[2], head, count, put_calls, get_calls, event_calls;

static bool matches(size_t slot, const uint64_t channel[3])
{
    assert(slot == 0);
    return channel[0] == 10 && channel[1] == 20 && channel[2] == 7;
}

static int32_t put(void *unused, size_t slot, const uint64_t channel[3],
                   const uint8_t *data, size_t size)
{
    (void)unused;
    pthread_mutex_lock(&registry);
    ++put_calls;
    int32_t result = -1;
    if (matches(slot, channel) && enabled) {
        result = size > sizeof(queue[0]) ? -5 : count == 2 ? -2 : 0;
        if (result == 0) {
            const size_t tail = (head + count) % 2;
            memcpy(queue[tail], data, size);
            sizes[tail] = size;
            ++count;
        }
    }
    pthread_mutex_unlock(&registry);
    return result;
}

static int32_t get(void *unused, size_t slot, const uint64_t channel[3],
                   uint8_t *data, size_t capacity)
{
    (void)unused;
    pthread_mutex_lock(&registry);
    ++get_calls;
    int32_t result = -1;
    if (matches(slot, channel) && enabled) {
        result = !count ? -3 : capacity < sizes[head] ? -5 : (int32_t)sizes[head];
        if (result >= 0) {
            memcpy(data, queue[head], sizes[head]);
            head = (head + 1) % 2;
            --count;
        }
    }
    pthread_mutex_unlock(&registry);
    return result;
}

static int32_t event_get(void *unused, size_t slot, uint64_t channel[3],
                         uint32_t *permissions)
{
    (void)unused;
    assert(slot == 0);
    pthread_mutex_lock(&registry);
    ++event_calls;
    const int32_t result = pending ? 0 : -3;
    if (pending) {
        channel[0] = 10; channel[1] = 20; channel[2] = 7;
        *permissions = enabled ? 3 : 0;
        pending = false;
    }
    pthread_mutex_unlock(&registry);
    return result;
}

static struct timespec deadline(void)
{
    struct timespec value;
    assert(clock_gettime(CLOCK_REALTIME, &value) == 0);
    value.tv_sec += 5;
    return value;
}

static bool event_ready(void *unused, size_t slot)
{
    (void)unused;
    assert(slot == 0);
    pthread_mutex_lock(&registry);
    const bool result = pending;
    if (pause_poll && !result) {
        pause_poll = false;
        poll_entered = true;
        pthread_cond_broadcast(&changed);
        const struct timespec until = deadline();
        while (!poll_released)
            assert(pthread_cond_timedwait(&changed, &registry, &until) == 0);
        /* Deliberately return the stale observation: the runtime must see its
         * retained wake flag and poll again before sleeping. */
    }
    pthread_mutex_unlock(&registry);
    return result;
}

static void notify(void *unused)
{
    (void)unused;
    core_runtime_state *state = runtime.lock(runtime.context, 0);
    const bool first_marker = state->errors_pending == 1;
    runtime.unlock(runtime.context, 0);
    pthread_mutex_lock(&registry);
    if (first_marker && !poll_entered) pause_poll = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&registry);
}

enum observation { PREPARED, COMPLETE, EXITED, DISPOSED };
static core_runtime_state await(enum observation observation)
{
    struct timespec until;
    assert(clock_gettime(CLOCK_MONOTONIC, &until) == 0);
    until.tv_sec += 5;
    for (;;) {
        core_runtime_state *locked = runtime.lock(runtime.context, 0);
        const core_runtime_state state = *locked;
        runtime.unlock(runtime.context, 0);
        if ((observation == PREPARED && state.prepared) ||
            (observation == COMPLETE && state.errors_pending == 2) ||
            (observation == EXITED && state.exited) ||
            (observation == DISPOSED && state.disposed)) return state;
        assert(!state.prepare_failed && !state.fault);
        struct timespec now;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        assert(now.tv_sec < until.tv_sec ||
               (now.tv_sec == until.tv_sec && now.tv_nsec < until.tv_nsec));
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    assert(core_wamr_profile(bytes, (size_t)length));
    const core_wamr_channels callbacks = {NULL, put, get, event_get, event_ready};
    assert(!core_wamr_attach_channels(&callbacks));
    assert(core_wamr_init(&runtime, notify, NULL));
    assert(!core_wamr_attach_channels(NULL));
    assert(!core_wamr_attach_channels(&(core_wamr_channels){0}));
    assert(core_wamr_attach_channels(&callbacks));
    assert(!core_wamr_attach_channels(&callbacks));
    assert(runtime.prepare(runtime.context, 0, bytes, (size_t)length,
                           NULL, 0, CORE_WAMR_ARENA_BYTES));
    await(PREPARED);
    core_runtime_state *state = runtime.lock(runtime.context, 0);
    state->start_deadline_us = UINT64_MAX;
    state->start_requested = true;
    runtime.unlock(runtime.context, 0);

    pthread_mutex_lock(&registry);
    const struct timespec until = deadline();
    while (!poll_entered)
        assert(pthread_cond_timedwait(&changed, &registry, &until) == 0);
    /* Configuration and invalid guest pointers never reach the callbacks. */
    assert(put_calls == 1 && get_calls == 1 && event_calls == 1);
    enabled = pending = true;
    core_wamr_channel_changed(0); /* Registry -> runtime lock order. */
    poll_released = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&registry);

    assert(await(COMPLETE).main_active);
    pthread_mutex_lock(&registry);
    assert(count == 0 && !pending);
    assert(put_calls == 7 && get_calls == 6 && event_calls == 3);
    pthread_mutex_unlock(&registry);
    state = runtime.lock(runtime.context, 0);
    state->cancel_requested = true;
    runtime.unlock(runtime.context, 0);
    assert(!await(EXITED).fault);
    state = runtime.lock(runtime.context, 0);
    state->dispose_requested = true;
    runtime.unlock(runtime.context, 0);
    await(DISPOSED);
    core_wamr_shutdown();
    puts("WAMR channel ABI, copied FIFO and wake/cancellation checks passed");
    return 0;
}

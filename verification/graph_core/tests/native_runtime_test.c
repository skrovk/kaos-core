#define _POSIX_C_SOURCE 200809L
#include "kaos_wamr.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Fixture observability is intentionally private to this synthetic sensor. */
extern atomic_uint core_native_fixture_calls;
extern atomic_uint core_native_fixture_active;

static core_node_runtime runtime;
static const core_native_imports granted = {
    .count = 1, .names = {"fixture_temperature_read"}
};

static uint64_t now_us(void)
{
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_nsec / 1000;
}

static void pause_once(void)
{
    const struct timespec pause = {.tv_nsec = 1000000};
    nanosleep(&pause, NULL);
}

static core_runtime_state snapshot(size_t slot)
{
    core_runtime_state state = *runtime.lock(runtime.context, slot);
    runtime.unlock(runtime.context, slot);
    return state;
}

enum observation { PREPARED, FAILED, INIT, DISPOSED };
static core_runtime_state await(size_t slot, enum observation wanted)
{
    const uint64_t deadline = now_us() + 5000000;
    for (;;) {
        core_runtime_state state = snapshot(slot);
        if ((wanted == PREPARED && state.prepared) ||
            (wanted == FAILED && state.prepare_failed) ||
            (wanted == INIT && state.init_pending) ||
            (wanted == DISPOSED && state.disposed)) return state;
        if (now_us() >= deadline) {
            fprintf(stderr, "native test timeout slot=%zu observation=%u "
                    "prepared=%u failed=%u exited=%u fault=%u\n", slot,
                    (unsigned)wanted, state.prepared, state.prepare_failed,
                    state.exited, state.fault);
            abort();
        }
        pause_once();
    }
}

static uint8_t *read_artifact(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    assert(fclose(file) == 0);
    *size = (size_t)length;
    return bytes;
}

static void prepare(size_t slot, const char *path, uint8_t mode,
                    const core_native_imports *imports)
{
    size_t size;
    uint8_t *bytes = read_artifact(path, &size);
    const bool accepted = imports ?
        runtime.prepare_with_imports(runtime.context, slot, bytes, size, &mode, 1,
                                     CORE_WAMR_ARENA_BYTES, imports) :
        runtime.prepare(runtime.context, slot, bytes, size, &mode, 1,
                        CORE_WAMR_ARENA_BYTES);
    assert(accepted);
}

static void start(size_t slot)
{
    core_runtime_state *state = runtime.lock(runtime.context, slot);
    state->start_deadline_us = UINT64_MAX;
    state->start_requested = true;
    runtime.unlock(runtime.context, slot);
}

static void dispose(size_t slot)
{
    core_runtime_state *state = runtime.lock(runtime.context, slot);
    state->cancel_requested = true;
    state->dispose_requested = true;
    runtime.unlock(runtime.context, slot);
    assert(await(slot, DISPOSED).disposed);
}

static void reject(size_t slot, const char *path, const core_native_imports *imports)
{
    const unsigned calls = atomic_load(&core_native_fixture_calls);
    prepare(slot, path, 0, imports);
    const core_runtime_state state = await(slot, FAILED);
    assert(!state.prepared && !state.init_pending && !state.configuration_active &&
           !state.main_active && atomic_load(&core_native_fixture_calls) == calls);
    dispose(slot);
}

static void malformed_grants(const char *path)
{
    core_native_imports bad = {.count = CORE_MAX_NATIVE_IMPORTS + 1};
    assert(!core_native_imports_valid(&bad));
    bad = (core_native_imports){.count = 1};
    assert(!core_native_imports_valid(&bad));
    memset(bad.names[0], 'a', sizeof(bad.names[0]));
    assert(!core_native_imports_valid(&bad));
    bad = (core_native_imports){.count = 1, .names = {"contains space"}};
    assert(!core_native_imports_valid(&bad));
    bad = (core_native_imports){.count = 2,
        .names = {"fixture_temperature_read", "fixture_temperature_read"}};
    assert(!core_native_imports_valid(&bad));
    size_t size;
    uint8_t *bytes = read_artifact(path, &size);
    assert(!runtime.prepare_with_imports(runtime.context, 0, bytes, size, NULL, 0,
                                        CORE_WAMR_ARENA_BYTES, &bad));
    free(bytes); /* Rejected dispatch never transfers artifact ownership. */
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    assert(core_wamr_init(&runtime, NULL, NULL));
    assert(runtime.prepare_with_imports);
    malformed_grants(argv[1]);
    size_t size;
    uint8_t *bytes = read_artifact(argv[1], &size);
    assert(!core_wamr_profile(bytes, size));
    assert(core_wamr_profile_with_imports(bytes, size, &granted));
    uint8_t *start_section = malloc(size + 3);
    assert(start_section);
    memcpy(start_section, bytes, size);
    memcpy(start_section + size, (const uint8_t[]){8, 1, 0}, 3);
    assert(!core_wamr_profile_with_imports(start_section, size + 3, &granted));
    free(start_section); /* A grant never permits implicit guest execution. */
    free(bytes);

    /* Queueing copies the grant before returning. A second resident node must
     * not gain permission from process-wide registration or its neighbour. */
    core_native_imports mutable = granted;
    prepare(0, argv[1], 0, &mutable);
    mutable = (core_native_imports){0};
    assert(await(0, PREPARED).prepared);
    assert(atomic_load(&core_native_fixture_calls) == 0);
    start(0);
    assert(!await(0, INIT).fault);
    assert(atomic_load(&core_native_fixture_calls) == 1);
    reject(1, argv[1], NULL);
    const core_native_imports unknown = {.count = 1, .names = {"unknown"}};
    reject(1, argv[1], &unknown);
    reject(1, argv[2], &granted); /* wrong module */
    reject(1, argv[3], &granted); /* unknown import name */
    reject(1, argv[4], &granted); /* actual Wasm type differs from registry */
    reject(1, argv[5], NULL); /* standard kaos import with the wrong type */
    assert(snapshot(0).main_active && !snapshot(0).fault);
    dispose(0);

    /* Cancellation during native acquisition wakes the call. Disposal is only
     * visible after that call has returned and released its guest address. */
    prepare(0, argv[1], 1, &granted);
    assert(await(0, PREPARED).prepared);
    start(0);
    await(0, INIT);
    const uint64_t deadline = now_us() + 5000000;
    while (!atomic_load(&core_native_fixture_active)) {
        assert(now_us() < deadline);
        pause_once();
    }
    assert(!snapshot(0).disposed);
    const uint64_t cancelled = now_us();
    dispose(0);
    assert(now_us() - cancelled < 1000000);
    assert(atomic_load(&core_native_fixture_active) == 0);
    reject(0, argv[1], NULL); /* slot reuse cannot inherit the previous grant */
    core_wamr_shutdown();
    puts("native registry, instance grants, ABI, arguments and cancellation passed");
    return 0;
}

#define _POSIX_C_SOURCE 200809L

#include "kaos_wamr.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static core_node_runtime runtime;
static uint8_t *fixture;
static size_t fixture_size;
static pthread_mutex_t changed_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned generation;
static unsigned pause_entry;
static bool entry_paused, entry_released;

static void notify(void *unused)
{
    (void)unused;
    core_runtime_state *state = runtime.lock(runtime.context, 0);
    const bool configuration = state->configuration_active;
    const bool main = state->main_active;
    runtime.unlock(runtime.context, 0);
    pthread_mutex_lock(&changed_mutex);
    ++generation;
    pthread_cond_broadcast(&changed);
    if ((pause_entry == 1 && configuration) || (pause_entry == 2 && main)) {
        entry_paused = true;
        pthread_cond_broadcast(&changed);
        struct timespec deadline;
        assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
        deadline.tv_sec += 5;
        while (!entry_released)
            assert(pthread_cond_timedwait(&changed, &changed_mutex, &deadline) == 0);
    }
    pthread_mutex_unlock(&changed_mutex);
}

static core_runtime_state snapshot(size_t slot)
{
    core_runtime_state value = *runtime.lock(runtime.context, slot);
    runtime.unlock(runtime.context, slot);
    return value;
}

enum observation { PREPARED, INIT, EXIT, DISPOSED, FAILED, CONFIGURING, ERROR_EVENT };

static core_runtime_state await(size_t slot, enum observation observation)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    for (;;) {
        pthread_mutex_lock(&changed_mutex);
        unsigned before = generation;
        pthread_mutex_unlock(&changed_mutex);
        core_runtime_state state = snapshot(slot);
        bool ready = observation == PREPARED ? state.prepared :
                     observation == INIT ? state.init_pending :
                     observation == EXIT ? state.exited :
                     observation == DISPOSED ? state.disposed :
                     observation == CONFIGURING ? state.configuration_active :
                     observation == ERROR_EVENT ? state.errors_pending != 0 : state.prepare_failed;
        if (ready) return state;
        pthread_mutex_lock(&changed_mutex);
        int result = 0;
        if (generation == before) result = pthread_cond_timedwait(&changed, &changed_mutex, &deadline);
        pthread_mutex_unlock(&changed_mutex);
        if (result != 0) {
            fprintf(stderr, "Timed out: slot=%zu observation=%d prepared=%d failed=%d exited=%d fault=%d\n",
                    slot, observation, state.prepared, state.prepare_failed, state.exited, state.fault);
            abort();
        }
    }
}

static void prepare(size_t slot, uint8_t mode, size_t budget)
{
    uint8_t *bytes = malloc(fixture_size);
    assert(bytes);
    memcpy(bytes, fixture, fixture_size);
    assert(runtime.prepare(runtime.context, slot, bytes, fixture_size, &mode, 1, budget));
}

static void start(size_t slot)
{
    core_runtime_state *state = runtime.lock(runtime.context, slot);
    state->start_deadline_us = UINT64_MAX;
    state->start_requested = true;
    runtime.unlock(runtime.context, slot);
}

static void cancel(size_t slot)
{
    core_runtime_state *state = runtime.lock(runtime.context, slot);
    state->cancel_requested = true;
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

static void cancel_at_call_entry(unsigned phase)
{
    prepare(0, 0, CORE_WAMR_ARENA_BYTES);
    await(0, PREPARED);
    pthread_mutex_lock(&changed_mutex);
    pause_entry = phase;
    entry_paused = false;
    entry_released = false;
    pthread_mutex_unlock(&changed_mutex);
    start(0);
    pthread_mutex_lock(&changed_mutex);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    while (!entry_paused)
        assert(pthread_cond_timedwait(&changed, &changed_mutex, &deadline) == 0);
    pthread_mutex_unlock(&changed_mutex);
    /* The worker has published execution but has not entered WAMR. The
     * termination flag must survive WAMR's call entry, including its first
     * bytecode instruction; neither configuration nor INIT may execute. */
    cancel(0);
    pthread_mutex_lock(&changed_mutex);
    pause_entry = 0;
    entry_released = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&changed_mutex);
    core_runtime_state result = await(0, EXIT);
    assert(!result.init_pending && !result.fault);
    dispose(0);
}

static void forbidden_exports(void)
{
    /* Valid tiny module with an additional alias for the main function. Each
     * implicit initialization spelling must fail before instantiation. */
    const uint8_t prefix[] = {0,97,115,109,1,0,0,0,
        1,10,2,96,2,127,127,1,127,96,0,0, 3,3,2,0,1, 5,4,1,1,1,1};
    const uint8_t code[] = {10,9,2,4,0,65,0,11,2,0,11};
    const char *forbidden[] = {"__post_instantiate", "__wasm_call_ctors", "_initialize"};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        uint8_t bytes[128];
        size_t n = sizeof(prefix);
        memcpy(bytes, prefix, n);
        bytes[n++] = 7;
        size_t size_index = n++;
        bytes[n++] = 3;
        const char *names[] = {"kaos_configure", "kaos_main", forbidden[i]};
        for (size_t j = 0; j < 3; ++j) {
            const size_t size = strlen(names[j]);
            bytes[n++] = (uint8_t)size;
            memcpy(bytes + n, names[j], size);
            n += size;
            bytes[n++] = 0;
            bytes[n++] = j == 0 ? 0 : 1;
        }
        bytes[size_index] = (uint8_t)(n - size_index - 1);
        memcpy(bytes + n, code, sizeof(code));
        n += sizeof(code);
        assert(!core_wamr_profile(bytes, n));
    }
}

static void concurrent_disposal(void)
{
    /* Last-exec deletion in one cluster overlaps the global cluster search
     * used to terminate another node. ThreadSanitizer caught the pinned
     * WAMR list-head race here before the build-copy locking correction. */
    for (unsigned cycle = 0; cycle < 200; ++cycle) {
        prepare(0, 4, CORE_WAMR_ARENA_BYTES);
        prepare(1, 0, CORE_WAMR_ARENA_BYTES);
        await(0, PREPARED);
        await(1, PREPARED);
        start(0);
        start(1);
        await(0, INIT);
        await(1, INIT);
        cancel(1);
        await(1, EXIT);
        core_runtime_state *closing = runtime.lock(runtime.context, 1);
        closing->dispose_requested = true;
        runtime.unlock(runtime.context, 1);
        cancel(0);
        await(0, EXIT);
        dispose(0);
        await(1, DISPOSED);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    fixture_size = (size_t)length;
    fixture = malloc(fixture_size);
    assert(fixture && fread(fixture, 1, fixture_size, file) == fixture_size);
    fclose(file);
    assert(core_wamr_profile(fixture, fixture_size));
    forbidden_exports();
    assert(!core_wamr_profile(fixture, fixture_size - 1));
    uint8_t *start_section = malloc(fixture_size + 3);
    assert(start_section);
    memcpy(start_section, fixture, fixture_size);
    memcpy(start_section + fixture_size, (uint8_t[]){8, 1, 0}, 3);
    assert(!core_wamr_profile(start_section, fixture_size + 3));
    free(start_section);
    uint8_t *unsupported_import = malloc(fixture_size);
    assert(unsupported_import);
    memcpy(unsupported_import, fixture, fixture_size);
    size_t module_name = 0;
    while (module_name + 4 < fixture_size &&
           memcmp(unsupported_import + module_name, "kaos", 4) != 0) ++module_name;
    assert(module_name + 4 < fixture_size);
    memcpy(unsupported_import + module_name, "evil", 4);
    assert(!core_wamr_profile(unsupported_import, fixture_size));
    free(unsupported_import);
    assert(core_wamr_init(&runtime, notify, NULL));

    /* Preparation is inert; INIT occurs only after explicit authorization. */
    prepare(0, 9, CORE_WAMR_ARENA_BYTES);
    core_runtime_state state = await(0, PREPARED);
    assert(!state.main_active && !state.init_pending && !state.exited);
    start(0);
    state = await(0, INIT);
    assert(state.main_active && !state.fault);
    cancel(0);
    state = await(0, EXIT);
    assert(!state.main_active && !state.fault && !state.disposed);
    dispose(0);

    cancel_at_call_entry(1);
    cancel_at_call_entry(2);

    /* Noncooperative compute-only main stops; the second node progresses. */
    prepare(0, 4, CORE_WAMR_ARENA_BYTES);
    prepare(1, 0, CORE_WAMR_ARENA_BYTES);
    await(0, PREPARED);
    await(1, PREPARED);
    start(0);
    start(1);
    await(0, INIT);
    await(1, INIT);
    cancel(0);
    await(0, EXIT);
    dispose(0);
    assert(snapshot(1).main_active);
    dispose(1);
    concurrent_disposal();

    /* Cancellation before startup suppresses both configuration and main. */
    prepare(0, 0, CORE_WAMR_ARENA_BYTES);
    await(0, PREPARED);
    core_runtime_state *commands = runtime.lock(runtime.context, 0);
    commands->start_requested = true;
    commands->cancel_requested = true;
    runtime.unlock(runtime.context, 0);
    dispose(0);
    assert(!snapshot(0).init_pending);

    /* Configuration can compute forever or wait inside a host import; both
     * are stopped without cooperative guest termination. */
    for (uint8_t mode = 1; mode <= 10; mode += 9) {
        prepare(0, mode, CORE_WAMR_ARENA_BYTES);
        await(0, PREPARED);
        start(0);
        await(0, CONFIGURING);
        await(0, ERROR_EVENT); /* actual guest configuration has executed */
        cancel(0);
        state = await(0, EXIT);
        assert(!state.configuration_active && !state.init_pending && !state.fault);
        dispose(0);
    }

    /* Expired authorization cannot enter configuration even if control's
     * next deadline tick is delayed. */
    prepare(0, 0, CORE_WAMR_ARENA_BYTES);
    await(0, PREPARED);
    commands = runtime.lock(runtime.context, 0);
    commands->start_requested = true;
    commands->start_deadline_us = 1;
    runtime.unlock(runtime.context, 0);
    state = await(0, EXIT);
    assert(!state.main_active && !state.init_pending);
    dispose(0);

    /* Config failure/trap and both sides of the INIT/exit ordering. */
    for (uint8_t mode = 2; mode <= 7; ++mode) {
        if (mode == 4) continue;
        prepare(0, mode, CORE_WAMR_ARENA_BYTES);
        await(0, PREPARED);
        start(0);
        state = await(0, EXIT);
        assert(!state.main_active);
        assert(state.fault == (mode == 2 || mode == 3 || mode == 7));
        assert(state.init_pending == (mode == 6 || mode == 7));
        dispose(0);
    }

    /* The generic ERROR FIFO cannot consume the reserved startup mailbox. */
    prepare(0, 8, CORE_WAMR_ARENA_BYTES);
    await(0, PREPARED);
    start(0);
    await(0, INIT);
    for (;;) {
        state = snapshot(0);
        if (state.errors_pending == CORE_WAMR_ERROR_CAPACITY) break;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    assert(state.main_active && state.init_pending && !state.fault);
    dispose(0);

    /* Repeated partial allocation failure and successful reuse reclaim arenas. */
    for (unsigned cycle = 0; cycle < 12; ++cycle) {
        prepare(0, 0, 128);
        await(0, FAILED);
        dispose(0);
        prepare(0, 0, CORE_WAMR_ARENA_BYTES);
        await(0, PREPARED);
        start(0);
        await(0, INIT);
        dispose(0);
    }
    core_wamr_shutdown();
    free(fixture);
    puts("WAMR restricted lifecycle: passed");
    return 0;
}

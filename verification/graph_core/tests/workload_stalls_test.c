#define _POSIX_C_SOURCE 200809L
#include "admission_fixture.h"
#include "kaos_endpoint.h"
#include "kaos_wamr.h"
#include <pthread.h>
#include <time.h>

/* Actual WAMR guests and actual endpoint queues; the local transfer scheduler
 * is controlled here. This fixture does not impersonate a graph coordinator. */
static core_node_runtime runtime;
static core_admission pool;
static core_endpoint_service endpoints;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static core_binding binding;
static unsigned produced, consumed, empty, full, dropped, mode;
static uint8_t *artifact;
static size_t artifact_size;

static void transfer(void)
{
    if (mode == 2 && consumed >= 8) return; /* named sender-drain barrier */
    core_data_frame frame;
    uint64_t owner;
    while (core_endpoint_next(&endpoints, &owner, &frame)) {
        CHECK(owner == 1);
        core_data_result result = core_endpoint_receive(&endpoints, 1, &frame);
        CHECK(result == CORE_DATA_OK || result == CORE_DATA_FULL);
        if (result == CORE_DATA_FULL) ++dropped;
    }
}

static int32_t put(void *unused, size_t slot, const uint64_t id[3],
                   const uint8_t *data, size_t size)
{
    (void)unused;
    CHECK(slot == 0);
    pthread_mutex_lock(&registry);
    const core_data_result result = core_channel_put(&endpoints,
        (core_instance_id){1}, (core_channel_id){id[0], id[1], id[2]}, data, size);
    CHECK(result == CORE_DATA_OK || result == CORE_DATA_FULL);
    if (result == CORE_DATA_OK) ++produced;
    else ++full;
    transfer();
    pthread_mutex_unlock(&registry);
    return result;
}

static int32_t get(void *unused, size_t slot, const uint64_t id[3],
                   uint8_t *data, size_t size)
{
    (void)unused;
    CHECK(slot == 1);
    pthread_mutex_lock(&registry);
    size_t written = 0;
    const core_data_result result = core_channel_get(&endpoints,
        (core_instance_id){2}, (core_channel_id){id[0], id[1], id[2]},
        data, size, &written);
    CHECK(result == CORE_DATA_OK || result == CORE_DATA_EMPTY);
    if (result == CORE_DATA_OK) ++consumed;
    else if (produced == 16) ++empty;
    transfer();
    pthread_mutex_unlock(&registry);
    return result == CORE_DATA_OK ? (int32_t)written : (int32_t)result;
}

static int32_t event_get(void *unused, size_t slot, uint64_t id[3], uint32_t *permissions)
{
    (void)unused;
    pthread_mutex_lock(&registry);
    core_channel_event event;
    const core_data_result result = core_channel_event_get(&endpoints,
        (core_instance_id){slot + 1}, &event);
    if (result == CORE_DATA_OK) {
        id[0] = event.channel.source; id[1] = event.channel.destination;
        id[2] = event.channel.service; *permissions = event.permissions;
    }
    pthread_mutex_unlock(&registry);
    return result;
}

static bool event_ready(void *unused, size_t slot)
{
    (void)unused;
    pthread_mutex_lock(&registry);
    bool pending = core_channel_event_pending(&endpoints, (core_instance_id){slot + 1});
    pthread_mutex_unlock(&registry);
    return pending;
}

static void nap(void) { nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL); }
static uint64_t now(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static core_runtime_state snapshot(size_t slot)
{
    core_runtime_state state = *runtime.lock(runtime.context, slot);
    runtime.unlock(runtime.context, slot);
    return state;
}

static void await(size_t slot, bool disposing)
{
    const uint64_t deadline = now() + 5000000000u;
    for (;;) {
        core_runtime_state state = snapshot(slot);
        CHECK(!state.fault && !state.prepare_failed);
        if (disposing ? state.disposed : state.prepared) return;
        CHECK(now() < deadline);
        nap();
    }
}

static void scenario(unsigned which)
{
    mode = which;
    produced = consumed = empty = full = dropped = 0;
    fixture_pool(&pool, 1);
    for (unsigned i = 0; i < 2; ++i) {
        fixture_request request = fixture_node(i + 1, 10 + 10 * i);
        const core_admission_record *record;
        CHECK(fixture_submit(&pool, &request, &record) == CORE_ADMIT_ACCEPTED);
        CHECK(core_admission_establish_node(&pool, (core_instance_id){i + 1}));
    }
    fixture_request edge = {.id = {3}, .scope = {
        .kind = CORE_REQUEST_ADD_EDGE, .channel_count = 1, .cleanup_allowance_us = 50}};
    edge.scope.bindings[0].channel = fixture_channel(10, 20, 1, 2, 1, 1);
    edge.scope.bindings[0].channel.payload_bytes = 64;
    const core_admission_record *record;
    CHECK(fixture_submit(&pool, &edge, &record) == CORE_ADMIT_ACCEPTED);
    binding = record->scope.bindings[0];
    core_endpoint_init(&endpoints, &pool, NULL, NULL);
    core_endpoint_step(&endpoints);
    CHECK(core_endpoint_activate(&endpoints, &binding, false));
    CHECK(core_endpoint_activate(&endpoints, &binding, true));
    const core_report before = record->evidence;

    for (size_t slot = 0; slot < 2; ++slot) {
        uint8_t *copy = malloc(artifact_size);
        CHECK(copy);
        memcpy(copy, artifact, artifact_size);
        uint8_t role = (uint8_t)(which == 0 ? slot : slot + 2);
        CHECK(runtime.prepare(runtime.context, slot, copy, artifact_size,
                              &role, 1, CORE_WAMR_ARENA_BYTES));
        await(slot, false);
        core_runtime_state *state = runtime.lock(runtime.context, slot);
        state->start_deadline_us = UINT64_MAX;
        state->start_requested = true;
        runtime.unlock(runtime.context, slot);
    }

    const uint64_t deadline = now() + 5000000000u;
    for (;;) {
        pthread_mutex_lock(&registry);
        bool reached = which == 0 ? empty >= 8 : which == 1 ? dropped >= 8 : full >= 8;
        if (reached) {
            CHECK(consumed > 0);
            CHECK(which == 0 ? produced == 16 && consumed + dropped == 16 : consumed == 8);
            CHECK(core_endpoint_facts(&endpoints, &binding, true) == 3);
            CHECK(core_endpoint_facts(&endpoints, &binding, false) == 3);
            CHECK(memcmp(&before, &record->evidence, sizeof(before)) == 0);
            printf("APP11 mode=%u PUT=%u GET=%u EMPTY=%u FULL=%u receiver_drop=%u\n",
                   which, produced, consumed, empty, full, dropped);
        }
        pthread_mutex_unlock(&registry);
        if (reached) break;
        CHECK(now() < deadline);
        nap();
    }
    for (size_t slot = 0; slot < 2; ++slot) {
        core_runtime_state state = snapshot(slot);
        CHECK(state.main_active && state.init_pending && !state.exited && !state.fault);
        core_runtime_state *locked = runtime.lock(runtime.context, slot);
        locked->cancel_requested = locked->dispose_requested = true;
        runtime.unlock(runtime.context, slot);
    }
    for (size_t slot = 0; slot < 2; ++slot) await(slot, true);
    core_endpoint_close(&endpoints, &binding);
    core_endpoint_step(&endpoints);
    CHECK((core_endpoint_facts(&endpoints, &binding, true) & 56) == 56);
    CHECK((core_endpoint_facts(&endpoints, &binding, false) & 56) == 56);
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e)
        CHECK(!pool.endpoints[e].occupied);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    CHECK(file && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    CHECK(size > 0 && fseek(file, 0, SEEK_SET) == 0);
    artifact_size = (size_t)size;
    artifact = malloc(artifact_size);
    CHECK(artifact && fread(artifact, 1, artifact_size, file) == artifact_size);
    fclose(file);
    CHECK(core_wamr_init(&runtime, NULL, NULL));
    const core_wamr_channels callbacks = {NULL, put, get, event_get, event_ready};
    CHECK(core_wamr_attach_channels(&callbacks));
    for (unsigned which = 0; which < 3; ++which) scenario(which);
    core_wamr_shutdown();
    free(artifact);
    puts("APP11: producer/consumer stalls, queue bounds and cancellable workers passed");
}

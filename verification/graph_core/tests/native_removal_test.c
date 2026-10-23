#define _POSIX_C_SOURCE 200809L
#include "admission_fixture.h"
#include "kaos_artifact.h"
#include "kaos_node.h"
#include "kaos_wamr.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* Compose the authorized request path, node lifecycle and actual native guest.
 * Artifact download is outside this check; its validated bytes are handed to
 * core_node_artifact with the same lifetime charge as the artifact adapter. */
extern atomic_uint core_native_fixture_active;

static core_admission pool;
static core_node_service nodes;
static core_node_runtime runtime;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static core_runtime_state *mailboxes[CORE_MAX_NODE_RESERVATIONS];
static bool cancellation_before[CORE_MAX_NODE_RESERVATIONS];
static bool cancelled_during_native, disposal_observed;

static uint64_t now_us(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static void pause_once(void)
{
    const struct timespec pause = {.tv_nsec = 1000000};
    nanosleep(&pause, NULL);
}

/* Observe the real service's mailbox publication without changing commands,
 * delaying the sensor or adding a production observation interface. */
static core_runtime_state *lock_mailbox(void *context, size_t slot)
{
    core_runtime_state *state = runtime.lock(context, slot);
    mailboxes[slot] = state;
    cancellation_before[slot] = state->cancel_requested;
    return state;
}

static void unlock_mailbox(void *context, size_t slot)
{
    const core_runtime_state *state = mailboxes[slot];
    if (!cancellation_before[slot] && state->cancel_requested &&
        atomic_load(&core_native_fixture_active) != 0)
        cancelled_during_native = true;
    if (state->disposed) {
        CHECK(atomic_load(&core_native_fixture_active) == 0);
        CHECK(!state->main_active && !state->configuration_active);
        disposal_observed = true;
    }
    runtime.unlock(context, slot);
}

static bool entry_lock(void *context, size_t slot)
{
    (void)context;
    pthread_mutex_lock(&registry);
    return core_node_start_allowed(&nodes, slot, now_us());
}

static void entry_unlock(void *context)
{
    (void)context;
    pthread_mutex_unlock(&registry);
}

static void step(void)
{
    pthread_mutex_lock(&registry);
    core_node_step(&nodes, now_us());
    pthread_mutex_unlock(&registry);
}

static uint8_t *read_artifact(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    CHECK(file && fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    CHECK(length > 0 && length <= CORE_MAX_ARTIFACT_BYTES);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length);
    CHECK(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    CHECK(fclose(file) == 0);
    *size = (size_t)length;
    return bytes;
}

static size_t addition_request(const uint8_t *artifact, size_t size,
                               uint8_t wire[CORE_MAX_REQUEST_BYTES])
{
    const uint8_t mode = 1; /* cancellable 100 ms acquisition, repeated */
    static const uint8_t name[] = "fixture_temperature_read";
    uint8_t body[CORE_MAX_REQUEST_BYTES];
    core_cbor_writer writer = {.data = body, .size = sizeof(body)};
    CHECK(core_cbor_write_array(&writer, 10));
    CHECK(core_cbor_write_uint(&writer, 10));
    CHECK(core_cbor_write_bytes(&writer, (const uint8_t *)"sensor", 6));
    CHECK(core_cbor_write_uint(&writer, size));
    CHECK(core_cbor_write_uint(&writer, core_artifact_crc32(artifact, size)));
    CHECK(core_cbor_write_uint(&writer, size + CORE_WAMR_ARENA_BYTES));
    CHECK(core_cbor_write_bytes(&writer, &mode, 1));
    CHECK(core_cbor_write_uint(&writer, 5000000));
    CHECK(core_cbor_write_uint(&writer, 2000000));
    CHECK(core_cbor_write_array(&writer, 0)); /* no ports */
    CHECK(core_cbor_write_array(&writer, 1));
    CHECK(core_cbor_write_bytes(&writer, name, sizeof(name) - 1));
    return fixture_envelope(wire, CORE_REQUEST_ADD_NODE_NO_EDGES, 1,
                            body, writer.offset);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    size_t artifact_size;
    uint8_t *artifact = read_artifact(argv[1], &artifact_size);
    const size_t node_bytes = artifact_size + CORE_WAMR_ARENA_BYTES;
    CHECK(core_admission_init(&pool, node_bytes, node_bytes));
    const uint64_t peers[] = {1};
    CHECK(core_admission_configure(&pool, 1, 99, peers, 1));
    CHECK(core_wamr_init(&runtime, NULL, NULL));
    nodes = (core_node_service){.admission = &pool, .runtime = runtime};
    nodes.runtime.lock = lock_mailbox;
    nodes.runtime.unlock = unlock_mailbox;
    CHECK(core_wamr_attach_entry_guard(NULL, entry_lock, entry_unlock));

    uint8_t wire[CORE_MAX_REQUEST_BYTES];
    size_t size = addition_request(artifact, artifact_size, wire);
    const core_admission_record *addition;
    CHECK(core_request_submit(&pool, 99, wire, size, now_us(), &addition) ==
          CORE_ADMIT_ACCEPTED);
    core_reserved_node *reservation = addition->node;
    CHECK(reservation && addition->request.imports.count == 1);
    CHECK(core_lifetime_charge(&reservation->lifetime, artifact_size));
    pthread_mutex_lock(&registry);
    CHECK(core_node_artifact(&nodes, addition->id, artifact, artifact_size, now_us()));
    pthread_mutex_unlock(&registry);
    const uint64_t startup_deadline = now_us() + 5000000;
    while (core_report_outcome(&addition->evidence) != CORE_OUTCOME_SUCCEEDED ||
           atomic_load(&core_native_fixture_active) == 0) {
        CHECK(now_us() < startup_deadline);
        CHECK(!(addition->evidence.objects[0] & CORE_OBJECT_FAILED));
        step();
        pause_once();
    }
    CHECK(reservation->occupied && reservation->established);
    CHECK(reservation->lifetime.bytes == node_bytes && pool.reserved_bytes == node_bytes);
    CHECK(!(addition->evidence.objects[0] & CORE_OBJECT_CLEANED));

    const fixture_request removal_request = {.id = {2}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 10,
        .instance = {1}, .allowance_us = 2000000}};
    size = fixture_encode(&removal_request, wire);
    const core_admission_record *removal;
    const uint64_t removal_started = now_us();
    pthread_mutex_lock(&registry);
    CHECK(core_request_submit(&pool, 99, wire, size, now_us(), &removal) ==
          CORE_ADMIT_ACCEPTED);
    CHECK(!(removal->evidence.objects[0] & CORE_OBJECT_CLEANED));
    core_node_step(&nodes, now_us());
    pthread_mutex_unlock(&registry);
    /* A scheduling miss is a failed check, never evidence of native removal. */
    CHECK(cancelled_during_native);

    while (core_report_outcome(&removal->evidence) != CORE_OUTCOME_SUCCEEDED) {
        CHECK(now_us() - removal_started < 1000000);
        step();
        if (removal->evidence.objects[0] & CORE_OBJECT_CLEANED) {
            CHECK(disposal_observed);
            CHECK(atomic_load(&core_native_fixture_active) == 0);
        }
        pause_once();
    }
    CHECK(now_us() - removal_started < 1000000);
    CHECK(disposal_observed && atomic_load(&core_native_fixture_active) == 0);
    const uint8_t closed = CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED;
    CHECK((removal->evidence.objects[0] & closed) == closed);
    CHECK((addition->evidence.objects[0] & closed) == closed);
    CHECK(core_report_outcome(&addition->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(pool.reserved_bytes == 0 && addition->node == NULL);
    CHECK(!reservation->occupied && core_lifetime_reclaimed(&reservation->lifetime));
    CHECK(reservation->lifetime.bytes == 0 && reservation->lifetime.allocations == 0);
    for (size_t slot = 0; slot < CORE_MAX_NODE_RESERVATIONS; ++slot)
        CHECK(!nodes.executions[slot].occupied);
    core_wamr_shutdown();
    puts("native removal: authorized cleanup cancels an active call before disposal/CLEANED");
    return 0;
}

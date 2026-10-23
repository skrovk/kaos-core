#define _POSIX_C_SOURCE 200809L
#include "admission_fixture.h"
#include "kaos_host.h"
#include <pthread.h>
#include <time.h>

typedef struct {
    core_host host;
    pthread_mutex_t registry, events;
    pthread_cond_t changed;
    pthread_t control;
    uint64_t control_time, worker_time;
    unsigned generation, entry_attempts, worker_checks;
    bool hold_entry;
    const core_host_packet *pending;
} device;
static uint8_t *guest;
static size_t guest_size;

static bool on_control(const device *d) { return pthread_equal(pthread_self(), d->control); }
static void lock(void *context)
{
    device *d = context;
    if (!on_control(d)) {
        CHECK(pthread_mutex_lock(&d->events) == 0);
        ++d->entry_attempts;
        CHECK(pthread_cond_broadcast(&d->changed) == 0);
        while (d->hold_entry) CHECK(pthread_cond_wait(&d->changed, &d->events) == 0);
        CHECK(pthread_mutex_unlock(&d->events) == 0);
    }
    CHECK(pthread_mutex_lock(&d->registry) == 0);
}
static void unlock(void *context) { CHECK(pthread_mutex_unlock(&((device *)context)->registry) == 0); }
static bool now(void *context, uint64_t *time)
{
    device *d = context;
    if (!on_control(d)) ++d->worker_checks;
    *time = on_control(d) ? d->control_time : d->worker_time;
    return true;
}
static void wake(void *context)
{
    device *d = context;
    CHECK(pthread_mutex_lock(&d->events) == 0);
    ++d->generation;
    CHECK(pthread_cond_broadcast(&d->changed) == 0);
    CHECK(pthread_mutex_unlock(&d->events) == 0);
}
static uint64_t retry(void *context) { (void)context; return CORE_COAP_ACK_TIMEOUT_US; }
static core_host_event receive_packet(void *context, core_host_class traffic_class, core_host_packet *packet)
{
    device *d = context;
    if (d->pending == NULL || d->pending->traffic_class != traffic_class) return CORE_HOST_EMPTY;
    *packet = *d->pending;
    d->pending = NULL;
    return CORE_HOST_PACKET;
}
static core_host_event wait_input(void *context, uint64_t deadline)
{
    (void)context; (void)deadline;
    return CORE_HOST_STOP;
}
static bool send_packet(void *context, core_host_class traffic_class, uint64_t peer,
                        const uint8_t *bytes, size_t size)
{
    device *d = context;
    (void)traffic_class; (void)peer; (void)bytes; (void)size;
    CHECK(on_control(d));
    CHECK(pthread_mutex_trylock(&d->registry) == 0);
    CHECK(pthread_mutex_unlock(&d->registry) == 0);
    return true; /* keep artifact requests pending */
}
static device *start(void)
{
    device *d = calloc(1, sizeof(*d));
    CHECK(d != NULL);
    d->control = pthread_self();
    CHECK(core_clock_now(&d->control_time));
    d->worker_time = d->control_time;
    CHECK(pthread_mutex_init(&d->registry, NULL) == 0);
    CHECK(pthread_mutex_init(&d->events, NULL) == 0);
    CHECK(pthread_cond_init(&d->changed, NULL) == 0);
    const core_host_platform p = {.context = d, .lock = lock, .unlock = unlock,
        .now = now, .wake = wake, .retry_us = retry, .wait = wait_input,
        .recv = receive_packet, .send = send_packet};
    const core_host_config config = {.local_device = 1, .authority = 99, .peers = {1},
        .peer_count = 1, .node_bytes = 131072,
        .byte_limit = CORE_MAX_NODE_RESERVATIONS * 131072u + CORE_MAX_ENDPOINT_RESERVATIONS * CORE_ENDPOINT_BYTES};
    CHECK(core_host_init(&d->host, &config, &p));
    return d;
}
static void finish(device *d)
{
    core_host_destroy(&d->host);
    CHECK(pthread_cond_destroy(&d->changed) == 0);
    CHECK(pthread_mutex_destroy(&d->events) == 0);
    CHECK(pthread_mutex_destroy(&d->registry) == 0);
    free(d);
}
static fixture_request node(uint64_t id, uint64_t logical)
{
    fixture_request r = fixture_node(id, logical);
    r.node.artifact_bytes = (uint32_t)guest_size;
    r.node.required_bytes = 131072;
    r.node.artifact_crc32 = core_artifact_crc32(guest, guest_size);
    r.node.addition_allowance_us = 30000000;
    return r;
}
static const core_admission_record *submit(device *d, const fixture_request *r)
{
    uint8_t bytes[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(r, bytes);
    const core_admission_record *record;
    lock(d);
    CHECK(core_request_submit(&d->host.admission, 99, bytes, size, d->control_time,
                              &record) == CORE_ADMIT_ACCEPTED);
    unlock(d);
    return record;
}
static size_t prepare(device *d, const core_admission_record *r)
{
    uint8_t *copy = malloc(guest_size);
    CHECK(copy != NULL);
    memcpy(copy, guest, guest_size);
    lock(d);
    CHECK(core_lifetime_charge(&r->node->lifetime, guest_size));
    CHECK(core_node_artifact(&d->host.nodes, r->id, copy, guest_size, d->control_time));
    const size_t slot = (size_t)(r->node - d->host.admission.nodes);
    unlock(d);
    return slot;
}
static void step(device *d, const core_host_packet *packet)
{
    uint64_t deadline;
    CHECK(d->pending == NULL);
    d->pending = packet;
    CHECK(core_host_step(&d->host, &deadline));
    CHECK(d->pending == NULL);
}
enum observation { PREPARED, INIT, EXITED, ENTRY_BLOCKED };
static void await(device *d, size_t slot, enum observation expected)
{
    struct timespec deadline;
    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    for (;;) {
        CHECK(pthread_mutex_lock(&d->events) == 0);
        const unsigned generation = d->generation;
        const bool attempted = d->entry_attempts != 0;
        CHECK(pthread_mutex_unlock(&d->events) == 0);
        core_node_runtime *runtime = &d->host.nodes.runtime;
        const core_runtime_state state = *runtime->lock(runtime->context, slot);
        runtime->unlock(runtime->context, slot);
        CHECK(!state.prepare_failed);
        if ((expected == PREPARED && state.prepared) || (expected == INIT && state.init_pending) ||
            (expected == EXITED && state.exited) || (expected == ENTRY_BLOCKED && attempted)) return;
        CHECK(pthread_mutex_lock(&d->events) == 0);
        if (d->generation == generation && !(expected == ENTRY_BLOCKED && d->entry_attempts))
            CHECK(pthread_cond_timedwait(&d->changed, &d->events, &deadline) == 0);
        CHECK(pthread_mutex_unlock(&d->events) == 0);
    }
}
static void worker_entry_does_not_schedule_other_operations(void)
{
    device *d = start();
    fixture_request first = node(1, 10), second = node(2, 20);
    second.node.addition_allowance_us = 100;
    const core_admission_record *running = submit(d, &first), *pending = submit(d, &second);
    const size_t slot = prepare(d, running);
    await(d, slot, PREPARED);
    /* Simulate time advancing between control dispatch and actual worker entry.
     * An unrelated artifact expires then, but only control may process it. */
    d->worker_time = pending->deadline_us;
    step(d, NULL);
    await(d, slot, INIT);
    lock(d);
    CHECK(d->worker_checks >= 2); /* configure and main both crossed the real guard */
    CHECK(!pending->cleanup_selected);
    CHECK(!(pending->evidence.objects[0] & CORE_OBJECT_EXPIRED));
    CHECK(!(running->evidence.history & CORE_HISTORY_STARTED));
    d->control_time = d->worker_time;
    unlock(d);
    step(d, NULL);
    CHECK(core_report_outcome(&pending->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(core_report_outcome(&running->evidence) == CORE_OUTCOME_SUCCEEDED);
    finish(d);
}
static void worker_entry_still_checks_its_own_deadline(void)
{
    device *d = start();
    const fixture_request request = node(1, 10);
    const core_admission_record *r = submit(d, &request);
    const size_t slot = prepare(d, r);
    await(d, slot, PREPARED);
    d->worker_time = r->deadline_us;
    step(d, NULL);
    await(d, slot, EXITED);
    lock(d);
    CHECK(!r->cleanup_selected); /* worker denied entry without scheduling cleanup */
    CHECK(!(r->evidence.history & CORE_HISTORY_STARTED));
    d->control_time = d->worker_time;
    unlock(d);
    step(d, NULL);
    CHECK(r->cleanup_selected && !(r->evidence.history & CORE_HISTORY_STARTED));
    finish(d);
}
static const core_admission_record *prepare_local_addition(device *d, size_t *slot)
{
    fixture_request peer = node(1, 10), request = node(2, 20);
    const core_admission_record *peer_record = submit(d, &peer);
    lock(d);
    /* An established local peer is fixture state, not a second guest runtime. */
    core_report *report = &d->host.admission.records[peer_record - d->host.admission.records].evidence;
    CHECK(core_report_observe(report, 0, CORE_OBJECT_READY) == CORE_REPORT_CHANGED);
    CHECK(core_report_record(report, CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH |
        CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS) == CORE_REPORT_CHANGED);
    CHECK(core_admission_establish_node(&d->host.admission, (core_instance_id){1}));
    unlock(d);
    request.scope.kind = CORE_REQUEST_ADD_NODE;
    request.scope.channel_count = 1;
    request.scope.bindings[0] = (core_binding){.creating_op = {2},
        .channel = fixture_channel(10, 20, 1, 2, 1, 1)};
    request.scope.bindings[0].channel.addition_allowance_us = 60000000;
    const core_predicate_token predicate = {.kind = CORE_PREDICATE_CHANNEL, .channel = 0};
    CHECK(core_predicate_init(&request.scope.predicate, &predicate, 1, 1));
    const core_admission_record *r = submit(d, &request);
    *slot = prepare(d, r);
    await(d, *slot, PREPARED);
    return r;
}
static void node_expiry_precedes_later_channel_activation(void)
{
    device *d = start();
    size_t slot;
    const core_admission_record *r = prepare_local_addition(d, &slot);
    lock(d);
    /* Pause at a legal state: endpoints prepared and node committed, with
     * activation still pending under a longer channel setup allowance. */
    core_edge_step(&d->host.edges, d->control_time);
    core_node_step(&d->host.nodes, d->control_time);
    CHECK(r->evidence.history & CORE_HISTORY_COMMIT);
    CHECK(!(r->evidence.history & CORE_HISTORY_DISPATCH));
    CHECK(core_edge_channels(r, CORE_OBJECT_ACTIVE) == 0);
    d->control_time = r->deadline_us;
    unlock(d);
    step(d, NULL);
    CHECK(r->cleanup_selected && (r->evidence.objects[0] & CORE_OBJECT_EXPIRED));
    CHECK(!(r->evidence.objects[1] & CORE_OBJECT_ACTIVE));
    CHECK(!(r->evidence.objects[2] & CORE_OBJECT_ACTIVE));
    CHECK(!(r->evidence.history & CORE_HISTORY_DISPATCH));
    finish(d);
}
static void local_progress_and_removal_before_entry(void)
{
    device *d = start();
    size_t slot;
    const core_admission_record *r = prepare_local_addition(d, &slot);
    CHECK(pthread_mutex_lock(&d->events) == 0);
    d->hold_entry = true;
    CHECK(pthread_mutex_unlock(&d->events) == 0);
    step(d, NULL); /* no input or remote peer to supply another scheduling turn */
    CHECK(core_edge_channels(r, CORE_OBJECT_ACTIVE) == 1);
    CHECK(r->evidence.history & CORE_HISTORY_DISPATCH);
    await(d, slot, ENTRY_BLOCKED);

    const fixture_request removal = {.id = {3}, .scope = {.kind = CORE_REQUEST_REMOVE_EDGE,
        .channel_count = 1, .allowance_us = 1000000, .bindings = {r->scope.bindings[0]}}};
    uint8_t body[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(&removal, body);
    const uint8_t format = 60;
    const core_coap_message message = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = 42, .token_size = 1, .token = {1}, .option_count = 2,
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)"graph", 5},
                    {CORE_COAP_CONTENT_FORMAT, &format, 1}}, .payload = body, .payload_size = size};
    core_host_packet packet = {.sender = 99};
    CHECK(core_coap_encode(&message, packet.bytes, sizeof(packet.bytes), &packet.size));
    step(d, &packet);
    CHECK(r->cleanup_selected && !core_node_start_allowed(&d->host.nodes, slot, d->control_time));
    CHECK(pthread_mutex_lock(&d->events) == 0);
    d->hold_entry = false;
    CHECK(pthread_cond_broadcast(&d->changed) == 0);
    CHECK(pthread_mutex_unlock(&d->events) == 0);
    await(d, slot, EXITED);
    CHECK(!(r->evidence.history & CORE_HISTORY_STARTED));
    finish(d);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    const long size = ftell(file);
    CHECK(size > 0 && fseek(file, 0, SEEK_SET) == 0);
    guest_size = (size_t)size;
    guest = malloc(guest_size);
    CHECK(guest != NULL && fread(guest, 1, guest_size, file) == guest_size);
    CHECK(fclose(file) == 0);
    worker_entry_does_not_schedule_other_operations();
    worker_entry_still_checks_its_own_deadline();
    local_progress_and_removal_before_entry();
    node_expiry_precedes_later_channel_activation();
    free(guest);
    puts("host scheduling: control ownership, read-only entry, local progress and removal passed");
}

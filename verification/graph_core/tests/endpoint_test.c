#include "admission_fixture.h"
#include "kaos_endpoint.h"
#include "kaos_node.h"

static unsigned wakes[CORE_MAX_NODE_RESERVATIONS];
static void wake(void *context, size_t slot)
{
    CHECK(context == wakes && slot < CORE_MAX_NODE_RESERVATIONS);
    ++wakes[slot];
}

static void add_node(core_admission *pool, uint64_t instance, uint64_t logical)
{
    const fixture_request request = fixture_node(instance, logical);
    const core_admission_record *record;
    CHECK(fixture_submit(pool, &request, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(pool, (core_instance_id){instance}));
}

static fixture_request edge(uint64_t id, uint64_t service, uint64_t destination_owner)
{
    fixture_request request = {.id = {id}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    request.scope.bindings[0].channel = fixture_channel(10, 20, 1, 2, 1, destination_owner);
    request.scope.bindings[0].channel.service = service;
    return request;
}

static const core_admission_record *add_edge(core_admission *pool, uint64_t id, uint64_t service)
{
    const fixture_request request = edge(id, service, 1);
    const core_admission_record *record;
    CHECK(fixture_submit(pool, &request, &record) == CORE_ADMIT_ACCEPTED);
    return record;
}

static void local_fixture(core_admission *pool, core_endpoint_service *service)
{
    fixture_pool(pool, 1);
    add_node(pool, 1, 10);
    add_node(pool, 2, 20);
    core_endpoint_init(service, pool, wake, wakes);
}

static core_reserved_endpoint *find_endpoint(core_admission *pool, bool sender)
{
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e)
        if (pool->endpoints[e].occupied && pool->endpoints[e].sender == sender)
            return &pool->endpoints[e];
    return NULL;
}

static void permissions_fifo_and_exact_frames(void)
{
    core_admission pool;
    core_endpoint_service service;
    local_fixture(&pool, &service);
    const core_admission_record *record = add_edge(&pool, 3, 7);
    const core_binding binding = record->scope.bindings[0];
    const core_channel_id channel = {10, 20, 7};
    uint8_t output[32];
    size_t written = 99;
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, "one", 3) == CORE_DATA_DISABLED);
    CHECK(core_channel_get(&service, (core_instance_id){2}, channel, output, sizeof(output),
                           &written) == CORE_DATA_DISABLED);
    CHECK(!core_endpoint_activate(&service, &binding, false));
    core_endpoint_step(&service);
    CHECK(record->local_facts[0] == CORE_OBJECT_READY && record->local_facts[1] == CORE_OBJECT_READY);
    CHECK(record->evidence.objects[0] == 0 && record->evidence.objects[1] == 0);
    CHECK(find_endpoint(&pool, true)->lifetime.bytes == CORE_ENDPOINT_BYTES);
    CHECK(core_endpoint_activate(&service, &binding, false));
    CHECK(core_channel_get(&service, (core_instance_id){2}, channel, output, sizeof(output),
                           &written) == CORE_DATA_EMPTY && written == 99);
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, "one", 3) == CORE_DATA_DISABLED);
    CHECK(core_endpoint_activate(&service, &binding, true));
    CHECK(core_channel_put(&service, (core_instance_id){2}, channel, "one", 3) == CORE_DATA_DISABLED);
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, NULL, 1) == CORE_DATA_INVALID);
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, output, 17) == CORE_DATA_SIZE);
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, "one", 3) == CORE_DATA_OK);
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, "two", 3) == CORE_DATA_OK);
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, "lost", 4) == CORE_DATA_FULL);

    core_data_frame frame;
    uint64_t destination;
    CHECK(core_endpoint_next(&service, &destination, &frame) && destination == 1);
    CHECK(core_endpoint_receive(&service, 2, &frame) == CORE_DATA_DISABLED);
    core_data_frame altered = frame;
    ++altered.creating_op.value;
    CHECK(core_endpoint_receive(&service, 1, &altered) == CORE_DATA_DISABLED);
    altered = frame;
    ++altered.source_instance.value;
    CHECK(core_endpoint_receive(&service, 1, &altered) == CORE_DATA_DISABLED);
    altered = frame;
    ++altered.destination_instance.value;
    CHECK(core_endpoint_receive(&service, 1, &altered) == CORE_DATA_DISABLED);
    altered = frame;
    ++altered.channel.service;
    CHECK(core_endpoint_receive(&service, 1, &altered) == CORE_DATA_DISABLED);
    altered = frame;
    altered.size = 17;
    CHECK(core_endpoint_receive(&service, 1, &altered) == CORE_DATA_SIZE);
    CHECK(core_endpoint_receive(&service, 1, &frame) == CORE_DATA_OK);
    CHECK(core_endpoint_next(&service, &destination, &frame));
    CHECK(core_endpoint_receive(&service, 1, &frame) == CORE_DATA_OK);
    CHECK(!core_endpoint_next(&service, &destination, &frame));

    /* Receiver saturation cannot turn successful put into operation failure. */
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, "lost", 4) == CORE_DATA_OK);
    CHECK(core_endpoint_next(&service, &destination, &frame));
    const core_report before = record->evidence;
    CHECK(core_endpoint_receive(&service, 1, &frame) == CORE_DATA_FULL);
    CHECK(memcmp(&before, &record->evidence, sizeof(before)) == 0);
    CHECK(core_channel_get(&service, (core_instance_id){2}, channel, output, 2, &written) == CORE_DATA_SIZE);
    CHECK(written == 99);
    CHECK(core_channel_get(&service, (core_instance_id){2}, channel, NULL, 3, &written) == CORE_DATA_INVALID);
    CHECK(core_channel_get(&service, (core_instance_id){2}, channel, output, sizeof(output),
                           &written) == CORE_DATA_OK && written == 3 && !memcmp(output, "one", 3));
    CHECK(core_channel_get(&service, (core_instance_id){2}, channel, output, sizeof(output),
                           &written) == CORE_DATA_OK && written == 3 && !memcmp(output, "two", 3));
    CHECK(core_channel_get(&service, (core_instance_id){2}, channel, output, sizeof(output),
                           &written) == CORE_DATA_EMPTY);
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, NULL, 0) == CORE_DATA_OK);
    CHECK(core_endpoint_next(&service, &destination, &frame) && frame.size == 0);
    CHECK(core_endpoint_receive(&service, 1, &frame) == CORE_DATA_OK);
    CHECK(core_channel_get(&service, (core_instance_id){2}, channel, NULL, 0, &written) == CORE_DATA_OK);
    CHECK(written == 0);
}

static void access_cleanup_and_replacement(void)
{
    core_admission pool;
    core_endpoint_service service;
    local_fixture(&pool, &service);
    const core_admission_record *record = add_edge(&pool, 3, 7);
    const core_binding binding = record->scope.bindings[0];
    const core_channel_id channel = {10, 20, 7};
    core_endpoint_step(&service);
    CHECK(core_endpoint_activate(&service, &binding, false));
    CHECK(core_endpoint_activate(&service, &binding, true));
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, "old", 3) == CORE_DATA_OK);
    core_data_frame delayed;
    uint64_t owner;
    CHECK(core_endpoint_next(&service, &owner, &delayed));

    core_reserved_endpoint *receiver = find_endpoint(&pool, false);
    core_access access;
    CHECK(core_access_acquire(&receiver->lifetime, &access));
    CHECK(core_admission_close_scope(&pool, record->id));
    CHECK(core_channel_put(&service, (core_instance_id){1}, channel, "new", 3) == CORE_DATA_DISABLED);
    CHECK(core_endpoint_receive(&service, 1, &delayed) == CORE_DATA_DISABLED);
    core_endpoint_step(&service);
    CHECK(!(record->local_facts[1] & CORE_OBJECT_STOPPED));
    CHECK(!(record->local_facts[0] & CORE_OBJECT_CLEANED));
    CHECK(receiver->occupied && receiver->lifetime.bytes == CORE_ENDPOINT_BYTES);
    CHECK(pool.nodes[0].occupied && pool.nodes[0].established && !pool.nodes[0].lifetime.closing);
    CHECK(pool.nodes[1].occupied && pool.nodes[1].established && !pool.nodes[1].lifetime.closing);
    CHECK(core_access_release(&receiver->lifetime, access));
    core_endpoint_step(&service);
    CHECK(record->local_facts[0] & CORE_OBJECT_CLEANED);
    CHECK(record->local_facts[1] & CORE_OBJECT_CLEANED);
    CHECK(pool.reserved_bytes == 2 * pool.node_bytes);
    CHECK(find_endpoint(&pool, true) == NULL && find_endpoint(&pool, false) == NULL);
    CHECK(core_channel_event_pending(&service, (core_instance_id){1}));
    CHECK(core_channel_event_pending(&service, (core_instance_id){2}));
    CHECK(core_admission_forget(&pool, (core_op_id){3}));

    /* A replacement coalesces the same logical channel's unread notification,
     * but delayed old data and old activation cannot target it. */
    record = add_edge(&pool, 4, 7);
    core_endpoint_step(&service);
    CHECK(!core_endpoint_activate(&service, &binding, true));
    CHECK(core_endpoint_activate(&service, &record->scope.bindings[0], false));
    CHECK(core_endpoint_activate(&service, &record->scope.bindings[0], true));
    CHECK(core_endpoint_receive(&service, 1, &delayed) == CORE_DATA_DISABLED);
    core_channel_event event;
    CHECK(core_channel_event_get(&service, (core_instance_id){1}, &event) == CORE_DATA_OK);
    CHECK(event.permissions == CORE_CHANNEL_PUT && event.channel.service == 7);
    CHECK(core_channel_event_get(&service, (core_instance_id){1}, &event) == CORE_DATA_EMPTY);
    CHECK(core_channel_event_get(&service, (core_instance_id){2}, &event) == CORE_DATA_OK);
    CHECK(event.permissions == CORE_CHANNEL_GET);
    CHECK(core_admission_close_scope(&pool, record->id));
    core_endpoint_step(&service);
    CHECK(core_channel_event_get(&service, (core_instance_id){1}, &event) == CORE_DATA_OK);
    CHECK(event.permissions == 0);
    CHECK(core_channel_event_get(&service, (core_instance_id){1}, &event) == CORE_DATA_EMPTY);
}

static void unread_events_bound_admission(void)
{
    core_admission pool;
    core_endpoint_service service;
    local_fixture(&pool, &service);
    for (size_t c = 0; c < CORE_MAX_REQUEST_CHANNELS; ++c) {
        const core_admission_record *record = add_edge(&pool, 10 + c, 100 + c);
        const core_op_id id = record->id;
        /* Removal before preparation still leaves a final disabled event. */
        CHECK(core_admission_close_scope(&pool, id));
        core_endpoint_step(&service);
        CHECK(record->local_facts[0] == (CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED));
        CHECK(record->local_facts[1] == record->local_facts[0]);
        CHECK(core_admission_forget(&pool, id));
        CHECK(pool.reserved_bytes == 2 * pool.node_bytes);
    }
    const core_admission before = pool;
    const fixture_request request = edge(99, 999, 1);
    const core_admission_record *record = NULL;
    CHECK(fixture_submit(&pool, &request, &record) == CORE_ADMIT_FULL);
    CHECK(record == NULL && !memcmp(&pool, &before, sizeof(pool)));
    core_channel_event event;
    CHECK(core_channel_event_get(&service, (core_instance_id){1}, &event) == CORE_DATA_OK);
    CHECK(event.permissions == 0);
    /* Each node needs its own event reservation. */
    CHECK(fixture_submit(&pool, &request, &record) == CORE_ADMIT_FULL);
    CHECK(core_channel_event_get(&service, (core_instance_id){2}, &event) == CORE_DATA_OK);
    CHECK(fixture_submit(&pool, &request, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(wakes[0] != 0 && wakes[1] != 0);
}

static void remote_copy_and_wire_boundaries(void)
{
    core_admission source, destination;
    core_endpoint_service sender_service, receiver_service;
    fixture_pool(&source, 1);
    fixture_pool(&destination, 2);
    add_node(&source, 1, 10);
    add_node(&destination, 2, 20);
    core_endpoint_init(&sender_service, &source, NULL, NULL);
    core_endpoint_init(&receiver_service, &destination, NULL, NULL);
    fixture_request request = edge(3, 7, 2);
    request.scope.bindings[0].channel.payload_bytes = CORE_MAX_PAYLOAD_BYTES;
    /* Rebuild manifests for this independent maximum-size fixture. */
    source.nodes[0].ports[0].max_payload = CORE_MAX_PAYLOAD_BYTES;
    destination.nodes[0].ports[1].max_payload = CORE_MAX_PAYLOAD_BYTES;
    uint8_t request_data[CORE_MAX_REQUEST_BYTES];
    const size_t request_size = fixture_encode(&request, request_data);
    const core_admission_record *sender_record, *receiver_record;
    CHECK(core_request_submit(&source, 99, request_data, request_size, 10, &sender_record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_endpoint_submit(&destination, 1, request_data, request_size, 10,
                               &receiver_record) == CORE_ADMIT_ACCEPTED);
    core_endpoint_step(&sender_service);
    core_endpoint_step(&receiver_service);
    CHECK(core_endpoint_activate(&receiver_service, &receiver_record->scope.bindings[0], false));
    CHECK(core_endpoint_activate(&sender_service, &sender_record->scope.bindings[0], true));
    uint8_t payload[CORE_MAX_PAYLOAD_BYTES];
    for (size_t b = 0; b < sizeof(payload); ++b) payload[b] = (uint8_t)b;
    const core_channel_id channel = {10, 20, 7};
    CHECK(core_channel_put(&sender_service, (core_instance_id){1}, channel,
                           payload, sizeof(payload)) == CORE_DATA_OK);
    core_data_frame frame, decoded;
    uint64_t owner;
    CHECK(core_endpoint_next(&sender_service, &owner, &frame) && owner == 2);
    memset(payload, 0xff, sizeof(payload));
    CHECK(frame.payload[0] == 0 && frame.payload[1] == 1);
    uint8_t wire[CORE_DATA_FRAME_BYTES + 1];
    size_t size;
    CHECK(!core_data_encode(&frame, wire, CORE_DATA_FRAME_BYTES - 1, &size));
    CHECK(core_data_encode(&frame, wire, CORE_DATA_FRAME_BYTES, &size));
    CHECK(size == CORE_DATA_FRAME_BYTES && wire[0] == CORE_SCHEMA_VERSION && wire[1] == 1);
    for (size_t cut = 0; cut < size; ++cut) CHECK(!core_data_decode(wire, cut, &decoded));
    CHECK(!core_data_decode(wire, size + 1, &decoded));
    wire[1] = 2;
    CHECK(!core_data_decode(wire, size, &decoded));
    wire[1] = 1;
    ++wire[0];
    CHECK(!core_data_decode(wire, size, &decoded));
    wire[0] = CORE_SCHEMA_VERSION;
    CHECK(core_data_decode(wire, size, &decoded));
    CHECK(core_endpoint_receive(&receiver_service, 1, &decoded) == CORE_DATA_OK);
    CHECK(core_channel_get(&receiver_service, (core_instance_id){2}, channel,
                           payload, sizeof(payload), &size) == CORE_DATA_OK);
    CHECK(size == sizeof(payload));
    for (size_t b = 0; b < sizeof(payload); ++b) CHECK(payload[b] == (uint8_t)b);
}

typedef struct {
    core_runtime_state states[CORE_MAX_NODE_RESERVATIONS];
    bool locked[CORE_MAX_NODE_RESERVATIONS];
} endpoint_runtime;

static bool runtime_prepare(void *context, size_t slot, uint8_t *artifact, size_t size,
                             const uint8_t *configuration, size_t configuration_size, size_t arena)
{
    endpoint_runtime *runtime = context;
    (void)configuration;
    CHECK(artifact != NULL && size == 16 && configuration_size == 0 && arena == 112);
    runtime->states[slot] = (core_runtime_state){.prepared = true};
    return true;
}

static core_runtime_state *runtime_lock(void *context, size_t slot)
{
    endpoint_runtime *runtime = context;
    CHECK(!runtime->locked[slot]);
    runtime->locked[slot] = true;
    return &runtime->states[slot];
}

static void runtime_unlock(void *context, size_t slot)
{
    endpoint_runtime *runtime = context;
    CHECK(runtime->locked[slot]);
    runtime->locked[slot] = false;
}

static void runtime_exit_reclaims_local_binding(void)
{
    /* Exercise both role orderings: the still-running peer may be visited
     * before the failed destination in the endpoint array. */
    for (size_t exited = 0; exited < 2; ++exited) {
        core_admission pool;
        fixture_pool(&pool, 1);
        core_endpoint_service service;
        core_endpoint_init(&service, &pool, wake, wakes);
        endpoint_runtime runtime = {0};
        core_node_service nodes = {.admission = &pool, .runtime = {.context = &runtime,
            .prepare = runtime_prepare, .lock = runtime_lock, .unlock = runtime_unlock}};
        uint8_t artifacts[2][16] = {{0}};
        const core_admission_record *creations[2];
        for (size_t n = 0; n < 2; ++n) {
            const fixture_request request = fixture_node(1 + n, 10 + 10 * n);
            CHECK(fixture_submit(&pool, &request, &creations[n]) == CORE_ADMIT_ACCEPTED);
            CHECK(core_lifetime_charge(&creations[n]->node->lifetime, sizeof(artifacts[n])));
            CHECK(core_node_artifact(&nodes, request.id, artifacts[n], sizeof(artifacts[n]), 15));
            core_node_step(&nodes, 20);
            runtime.states[n].main_active = runtime.states[n].init_pending = true;
            core_node_step(&nodes, 25);
            CHECK(core_report_outcome(&creations[n]->evidence) == CORE_OUTCOME_SUCCEEDED);
        }
        const core_admission_record *record = add_edge(&pool, 3, 7);
        const core_binding binding = record->scope.bindings[0];
        core_endpoint_step(&service);
        CHECK(core_endpoint_activate(&service, &binding, false));
        CHECK(core_endpoint_activate(&service, &binding, true));
        core_access held;
        core_reserved_endpoint *endpoint = find_endpoint(&pool, exited == 0);
        CHECK(core_access_acquire(&endpoint->lifetime, &held));
        core_data_frame delayed = {.channel = {10, 20, 7}, .creating_op = {3},
            .source_instance = {1}, .destination_instance = {2}, .size = 1, .payload = {42}};

        runtime.states[exited].main_active = false;
        runtime.states[exited].exited = runtime.states[exited].fault = true;
        runtime.states[exited].disposed = true;
        core_node_step(&nodes, 40);
        CHECK(creations[exited]->node != NULL); /* endpoint association still pins registry */
        CHECK(core_lifetime_reclaimed(&creations[exited]->node->lifetime));
        core_endpoint_step(&service);
        CHECK(core_endpoint_receive(&service, 1, &delayed) == CORE_DATA_DISABLED);
        CHECK(core_channel_put(&service, (core_instance_id){1}, delayed.channel, "x", 1) == CORE_DATA_DISABLED);
        CHECK(!(record->local_facts[0] & CORE_OBJECT_CLEANED));
        CHECK(!(record->local_facts[1] & CORE_OBJECT_CLEANED));
        CHECK(!runtime.states[1 - exited].cancel_requested && runtime.states[1 - exited].main_active);
        CHECK(core_access_release(&endpoint->lifetime, held));
        core_endpoint_step(&service);
        CHECK(record->local_facts[0] & CORE_OBJECT_CLEANED);
        CHECK(record->local_facts[1] & CORE_OBJECT_CLEANED);
        /* Endpoint release is followed by a bounded node pass, without waiting
         * for another completion that may never arrive. */
        core_node_step(&nodes, 41);
        CHECK(creations[exited]->node == NULL && !pool.nodes[exited].occupied);
        CHECK(core_report_outcome(&creations[exited]->evidence) == CORE_OUTCOME_SUCCEEDED);
        CHECK(creations[exited]->evidence.objects[0] & CORE_OBJECT_CLEANED);
        CHECK(pool.nodes[1 - exited].occupied && pool.nodes[1 - exited].established);
        CHECK(pool.reserved_bytes == pool.node_bytes);
        for (size_t e = 0; e < CORE_MAX_REQUEST_CHANNELS; ++e)
            CHECK(!pool.nodes[exited].notifications[e].occupied);
        CHECK(!core_channel_event_pending(&service, (core_instance_id){1 + exited}));

        const fixture_request replacement = fixture_node(4, 10 + 10 * exited);
        const core_admission_record *new_node;
        CHECK(fixture_submit(&pool, &replacement, &new_node) == CORE_ADMIT_ACCEPTED);
        CHECK(new_node->node == &pool.nodes[exited]);
        CHECK(!core_channel_event_pending(&service, (core_instance_id){4}));
        CHECK(core_endpoint_receive(&service, 1, &delayed) == CORE_DATA_DISABLED);
        CHECK(!core_endpoint_activate(&service, &binding, exited == 0));
    }
}

static void close_event_and_retained_descriptor(void)
{
    core_admission pool;
    core_endpoint_service service;
    local_fixture(&pool, &service);
    const core_admission_record *record = add_edge(&pool, 3, 7);
    const core_binding binding = record->scope.bindings[0];
    core_endpoint_step(&service);
    CHECK(core_endpoint_activate(&service, &binding, false));
    CHECK(core_endpoint_activate(&service, &binding, true));
    core_endpoint_close(&service, &binding);
    core_channel_event event;
    CHECK(core_channel_event_get(&service, (core_instance_id){1}, &event) == CORE_DATA_OK);
    CHECK(event.permissions == 0); /* close publishes disable before a later cleanup pass */
    CHECK(core_channel_event_get(&service, (core_instance_id){2}, &event) == CORE_DATA_OK);
    CHECK(event.permissions == 0);
    core_endpoint_step(&service);
    CHECK(find_endpoint(&pool, true) == NULL && find_endpoint(&pool, false) == NULL);
    fixture_request removal = {.id = {4}, .scope = {.kind = CORE_REQUEST_REMOVE_EDGE,
        .channel_count = 1, .allowance_us = 50}};
    removal.scope.bindings[0] = binding;
    ++removal.scope.bindings[0].channel.payload_bytes;
    const core_admission before = pool;
    const core_admission_record *result = NULL;
    CHECK(fixture_submit(&pool, &removal, &result) == CORE_ADMIT_CONFLICT);
    CHECK(result == NULL && !memcmp(&pool, &before, sizeof(pool)));
    removal.scope.bindings[0] = binding;
    CHECK(fixture_submit(&pool, &removal, &result) == CORE_ADMIT_ACCEPTED);
    core_endpoint_step(&service);
    CHECK(result->local_facts[0] == (CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED));
    CHECK(result->local_facts[1] == result->local_facts[0]);
}

int main(void)
{
    permissions_fifo_and_exact_frames();
    access_cleanup_and_replacement();
    unread_events_bound_admission();
    remote_copy_and_wire_boundaries();
    runtime_exit_reclaims_local_binding();
    close_event_and_retained_descriptor();
    puts("endpoints: permission gates, FIFO/drop-incoming, exact copied frames, cleanup and bounded events passed");
    return 0;
}

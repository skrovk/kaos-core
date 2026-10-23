#include "admission_fixture.h"
#include "kaos_endpoint.h"

static size_t removal_command(uint8_t *bytes, uint64_t id, uint8_t index,
                              uint64_t target, const core_binding *binding)
{
    core_cbor_writer writer = {.data = bytes, .size = 160};
    CHECK(core_cbor_write_array(&writer, 6));
    CHECK(core_cbor_write_uint(&writer, CORE_SCHEMA_VERSION));
    CHECK(core_cbor_write_uint(&writer, CORE_REMOVAL_COMMAND_KIND));
    CHECK(core_cbor_write_uint(&writer, id));
    CHECK(core_cbor_write_uint(&writer, index));
    CHECK(core_cbor_write_uint(&writer, target));
    fixture_write_binding(&writer, binding);
    return writer.offset;
}

static core_admit_result remove_channel(core_admission *pool, uint64_t sender,
    uint64_t id, uint8_t index, uint64_t target, const core_binding *binding,
    const core_admission_record **out)
{
    uint8_t bytes[160];
    const size_t size = removal_command(bytes, id, index, target, binding);
    return core_endpoint_removal_submit(pool, sender, bytes, size, out);
}

static core_admit_result prepare_channel(core_admission *pool, uint8_t index,
    const core_channel *channel, const core_admission_record **out)
{
    uint8_t bytes[160];
    core_cbor_writer writer = {.data = bytes, .size = sizeof(bytes)};
    CHECK(core_cbor_write_array(&writer, 5));
    CHECK(core_cbor_write_uint(&writer, CORE_SCHEMA_VERSION));
    CHECK(core_cbor_write_uint(&writer, CORE_CHANNEL_COMMAND_KIND));
    CHECK(core_cbor_write_uint(&writer, 100));
    CHECK(core_cbor_write_uint(&writer, index));
    fixture_write_channel(&writer, channel);
    return core_endpoint_channel_submit(pool, 2, bytes, writer.offset, 0, false, out);
}

static void compact_identity_and_fences(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const fixture_request node = fixture_node(20, 20);
    const core_admission_record *out;
    CHECK(fixture_submit(&pool, &node, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){20}));
    const core_binding binding = {fixture_channel(100, 20, 100, 20, 2, 1), {100}};
    CHECK(prepare_channel(&pool, 0, &binding.channel, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(remove_channel(&pool, 2, 200, 2, 100, &binding, &out) == CORE_ADMIT_ACCEPTED);
    const core_admission_record *removal = out;
    CHECK(removal->id.value == 200 && removal->scope.instance.value == 100);
    CHECK(removal->scope.kind == CORE_REQUEST_REMOVE_NODE && removal->endpoint_only);
    CHECK(removal->deadline_us == 0 && removal->cleanup_selected);
    CHECK(removal->channel_known == 4 && removal->channel_cleanup == 4);
    CHECK(pool.endpoints[0].lifetime.closing);
    CHECK(!pool.nodes[0].lifetime.closing);
    core_admission before = pool;
    CHECK(remove_channel(&pool, 2, 200, 2, 100, &binding, &out) == CORE_ADMIT_DUPLICATE);
    CHECK(out == removal && memcmp(&before, &pool, sizeof(pool)) == 0);
    core_binding wrong = binding;
    wrong.channel.payload_bytes = 17;
    out = NULL;
    CHECK(remove_channel(&pool, 2, 200, 2, 100, &wrong, &out) == CORE_ADMIT_CONFLICT);
    CHECK(out == NULL && memcmp(&before, &pool, sizeof(pool)) == 0);
    CHECK(remove_channel(&pool, 2, 200, 1, 100, &binding, &out) == CORE_ADMIT_CONFLICT);
    CHECK(remove_channel(&pool, 3, 200, 2, 100, &binding, &out) == CORE_ADMIT_UNAUTHORIZED);
    CHECK(remove_channel(&pool, 2, 100, 2, 100, &binding, &out) == CORE_ADMIT_INVALID);
    wrong.channel.service = 8;
    CHECK(prepare_channel(&pool, 1, &wrong.channel, &out) == CORE_ADMIT_CONFLICT);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);

    core_endpoint_service endpoints;
    core_endpoint_init(&endpoints, &pool, NULL, NULL);
    core_endpoint_step(&endpoints);
    CHECK(!pool.endpoints[0].occupied);
    CHECK(removal->local_facts[6] & CORE_OBJECT_CLEANED);
    CHECK(core_admission_find(&pool, (core_op_id){100})->local_facts[2] & CORE_OBJECT_CLEANED);
    CHECK(prepare_channel(&pool, 0, &binding.channel, &out) == CORE_ADMIT_DUPLICATE);
    CHECK(!pool.endpoints[0].occupied);

    /* An owner with discharged local work must not retain fictitious remote
     * obligations which permanently forbid removing its surviving local node. */
    const fixture_request local_remove = {.id = {201}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 20,
        .instance = {20}, .allowance_us = 50}};
    CHECK(fixture_submit(&pool, &local_remove, &out) == CORE_ADMIT_ACCEPTED);
}

static void never_prepared_and_capacity(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const core_binding binding = {fixture_channel(100, 20, 100, 20, 2, 1), {100}};
    const core_admission_record *out;
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i)
        CHECK(remove_channel(&pool, 2, 200 + i, 0, 100, &binding, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(pool.reserved_bytes == 0);
    core_admission before = pool;
    CHECK(remove_channel(&pool, 2, 500, 0, 100, &binding, &out) == CORE_ADMIT_FULL);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);
    CHECK(remove_channel(&pool, 2, 200, 0, 100, &binding, &out) == CORE_ADMIT_DUPLICATE);
    core_endpoint_service endpoints;
    core_endpoint_init(&endpoints, &pool, NULL, NULL);
    core_endpoint_step(&endpoints);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i)
        CHECK(pool.records[i].local_facts[2] & CORE_OBJECT_CLEANED);
}

static void reclaimed_remote_incidence(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const fixture_request node = fixture_node(10, 10);
    const core_admission_record *out;
    CHECK(fixture_submit(&pool, &node, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){10}));
    fixture_request edge = {.id = {100}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    edge.scope.bindings[0] = (core_binding){fixture_channel(10, 20, 10, 20, 1, 2), {100}};
    CHECK(fixture_submit(&pool, &edge, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_close_channel(&pool, edge.id, 0));
    core_endpoint_service endpoints;
    core_endpoint_init(&endpoints, &pool, NULL, NULL);
    core_endpoint_step(&endpoints);
    CHECK(!pool.endpoints[0].occupied);
    fixture_request removal = {.id = {200}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 10,
        .instance = {10}, .allowance_us = 50}};
    core_admission before = pool;
    CHECK(fixture_submit(&pool, &removal, &out) == CORE_ADMIT_CONFLICT);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);
    removal.scope.kind = CORE_REQUEST_REMOVE_NODE;
    removal.scope.channel_count = 1;
    removal.scope.bindings[0] = edge.scope.bindings[0];
    CHECK(fixture_submit(&pool, &removal, &out) == CORE_ADMIT_ACCEPTED);
    core_admission_record *observer = &pool.records[out - pool.records];
    CHECK(core_report_observe(&observer->evidence, 2,
        CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED) == CORE_REPORT_CHANGED);
    removal.id.value = 201;
    removal.scope.kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES;
    removal.scope.channel_count = 0;
    CHECK(fixture_submit(&pool, &removal, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_find(&pool, edge.id)->evidence.objects[1] == 0);
}

static void retained_incidence_capacity(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    fixture_request node = fixture_node(100, 10);
    node.scope.kind = CORE_REQUEST_ADD_NODE;
    node.scope.channel_count = CORE_MAX_REQUEST_CHANNELS;
    node.scope.predicate.count = 1;
    node.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_TRUE, 0};
    for (size_t c = 0; c < CORE_MAX_REQUEST_CHANNELS; ++c) {
        node.scope.bindings[c].channel = fixture_channel(10, 20, 100, 20, 1, 2);
        node.scope.bindings[c].channel.service = c;
    }
    const core_admission_record *out;
    CHECK(fixture_submit(&pool, &node, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){100}));
    for (size_t c = 0; c < CORE_MAX_REQUEST_CHANNELS; ++c)
        CHECK(core_admission_close_channel(&pool, node.id, (uint8_t)c));
    core_endpoint_service endpoints;
    core_endpoint_init(&endpoints, &pool, NULL, NULL);
    core_endpoint_step(&endpoints);
    core_channel_event event;
    while (core_channel_event_get(&endpoints, (core_instance_id){100}, &event) == CORE_DATA_OK) {}
    CHECK(pool.reserved_bytes == pool.node_bytes);
    fixture_request edge = {.id = {200}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    edge.scope.bindings[0].channel = fixture_channel(10, 20, 100, 20, 1, 2);
    edge.scope.bindings[0].channel.service = 100;
    core_admission before = pool;
    CHECK(fixture_submit(&pool, &edge, &out) == CORE_ADMIT_FULL);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);
    /* Accepted remote closure discharges the remaining role and makes exactly
     * one incidence slot reusable; local release alone above did not. */
    CHECK(core_report_observe(&pool.records[0].evidence, 2,
        CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED) == CORE_REPORT_CHANGED);
    CHECK(fixture_submit(&pool, &edge, &out) == CORE_ADMIT_ACCEPTED);
}

static void largest_command(void)
{
    core_admission pool;
    const uint64_t peers[] = {UINT64_MAX, UINT64_MAX - 1};
    CHECK(core_admission_init(&pool, 4096, 4096));
    CHECK(core_admission_configure(&pool, UINT64_MAX, 99, peers, 2));
    core_binding binding = {fixture_channel(UINT64_MAX, UINT64_MAX - 1,
        UINT64_MAX - 2, UINT64_MAX - 3, UINT64_MAX - 1, UINT64_MAX), {UINT64_MAX - 4}};
    binding.channel.service = UINT64_MAX;
    binding.channel.source_port = binding.channel.destination_port = binding.channel.type = UINT16_MAX;
    binding.channel.payload_bytes = CORE_MAX_PAYLOAD_BYTES;
    binding.channel.queue_messages = CORE_MAX_QUEUE_MESSAGES;
    binding.channel.addition_allowance_us = UINT64_MAX;
    uint8_t bytes[160];
    const size_t size = removal_command(bytes, UINT64_MAX - 5,
        CORE_MAX_REQUEST_CHANNELS - 1, UINT64_MAX - 2, &binding);
    CHECK(size == 118);
    const core_admission_record *out;
    CHECK(core_endpoint_removal_submit(&pool, UINT64_MAX - 1, bytes, size,
        &out) == CORE_ADMIT_ACCEPTED);
    CHECK(out->deadline_us == 0 && pool.reserved_bytes == 0);
}

int main(void)
{
    compact_identity_and_fences();
    never_prepared_and_capacity();
    reclaimed_remote_incidence();
    retained_incidence_capacity();
    largest_command();
    puts("P6 removal admission passed");
    return 0;
}

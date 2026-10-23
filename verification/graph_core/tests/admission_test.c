#include "admission_fixture.h"

static void all_six_and_cleanup(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const core_admission_record *record;
    fixture_request first = fixture_node(1, 10);
    CHECK(fixture_submit(&pool, &first, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->deadline_us == 110 && record->scope.object_count == 1);
    CHECK(record->evidence.object_count == 1 && record->evidence.revision == 0);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){1}));

    fixture_request second = fixture_node(2, 20);
    second.scope.kind = CORE_REQUEST_ADD_NODE;
    second.scope.channel_count = 1;
    second.scope.bindings[0].channel = fixture_channel(10, 20, 1, 2, 1, 1);
    second.scope.predicate.count = 1;
    second.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_CHANNEL, 0};
    CHECK(fixture_submit(&pool, &second, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->scope.object_count == 3 && pool.reserved_bytes == 10240);
    CHECK(pool.endpoints[0].occupied && pool.endpoints[1].occupied);
    CHECK(pool.endpoints[0].sender && !pool.endpoints[1].sender);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){2}));

    fixture_request remove_first = {.id = {3}, .scope = {.kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES,
        .logical_node_id = 10, .instance = {1}, .allowance_us = 50}};
    core_admission before = pool;
    const core_admission_record *unchanged = record;
    CHECK(fixture_submit(&pool, &remove_first, &record) == CORE_ADMIT_CONFLICT);
    CHECK(record == unchanged && memcmp(&before, &pool, sizeof(pool)) == 0);
    remove_first.scope.kind = CORE_REQUEST_REMOVE_NODE;
    remove_first.scope.channel_count = 1;
    remove_first.scope.bindings[0] = (core_binding){second.scope.bindings[0].channel, {2}};
    CHECK(fixture_submit(&pool, &remove_first, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(!core_admission_forget(&pool, (core_op_id){3}));
    CHECK(core_admission_close_scope(&pool, (core_op_id){3}));
    CHECK(!core_admission_release_node(&pool, (core_op_id){1}));
    CHECK(core_admission_release_endpoints(&pool, (core_op_id){2}));
    CHECK(core_admission_release_node(&pool, (core_op_id){1}));
    CHECK(core_admission_forget(&pool, (core_op_id){1}));
    CHECK(core_admission_forget(&pool, (core_op_id){3}));
    CHECK(pool.nodes[1].occupied && !pool.nodes[1].lifetime.closing);
    CHECK(pool.reserved_bytes == 4096);

    fixture_request edge = {.id = {4}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    edge.scope.bindings[0].channel = fixture_channel(20, 30, 2, 9, 1, 2);
    CHECK(fixture_submit(&pool, &edge, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->scope.object_count == 2 && pool.reserved_bytes == 5120);
    fixture_request remove_edge = {.id = {5}, .scope = {.kind = CORE_REQUEST_REMOVE_EDGE,
        .channel_count = 1, .allowance_us = 50}};
    remove_edge.scope.bindings[0] = (core_binding){edge.scope.bindings[0].channel, {4}};
    CHECK(fixture_submit(&pool, &remove_edge, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_close_scope(&pool, (core_op_id){5}));
    CHECK(core_admission_release_endpoints(&pool, (core_op_id){4}));
    CHECK(core_admission_forget(&pool, (core_op_id){4}));
    CHECK(core_admission_forget(&pool, (core_op_id){5}));
    CHECK(!pool.nodes[1].lifetime.closing);

    fixture_request remove_second = {.id = {6}, .scope = {.kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES,
        .logical_node_id = 20, .instance = {2}, .allowance_us = 50}};
    CHECK(fixture_submit(&pool, &remove_second, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_close_scope(&pool, (core_op_id){6}));
    CHECK(core_admission_release_node(&pool, (core_op_id){2}));
    CHECK(core_admission_forget(&pool, (core_op_id){2}));
    CHECK(core_admission_forget(&pool, (core_op_id){6}));
    CHECK(pool.reserved_bytes == 0);
}

static void precreation_fences_and_capacity(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const core_admission_record *record;
    fixture_request remove = {.id = {8}, .scope = {.kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES,
        .logical_node_id = 10, .instance = {7}, .allowance_us = 50}};
    CHECK(fixture_submit(&pool, &remove, &record) == CORE_ADMIT_ACCEPTED);
    fixture_request add = fixture_node(7, 10);
    CHECK(fixture_submit(&pool, &add, &record) == CORE_ADMIT_CONFLICT);
    CHECK(pool.reserved_bytes == 0);
    CHECK(core_admission_forget(&pool, (core_op_id){8}));
    /* Q05 characterization: after the caller retires all rejection history,
     * an obsolete construction can be admitted. */
    CHECK(fixture_submit(&pool, &add, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_close_scope(&pool, (core_op_id){7}));
    CHECK(core_admission_release_node(&pool, (core_op_id){7}));
    for (uint64_t i = 10; i < 13; ++i) {
        remove.id.value = i;
        CHECK(fixture_submit(&pool, &remove, &record) == CORE_ADMIT_ACCEPTED);
    }
    core_admission before = pool;
    remove.id.value = 13;
    CHECK(fixture_submit(&pool, &remove, &record) == CORE_ADMIT_FULL);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);
}

static void remote_reservations_and_cancellation(void)
{
    core_admission owner;
    fixture_pool(&owner, 2);
    const core_admission_record *record;
    fixture_request local = fixture_node(1, 10);
    CHECK(fixture_submit(&owner, &local, &record) == CORE_ADMIT_ACCEPTED);
    fixture_request remote = fixture_node(2, 20);
    remote.scope.kind = CORE_REQUEST_ADD_NODE;
    remote.scope.channel_count = 1;
    remote.scope.bindings[0].channel = fixture_channel(20, 10, 2, 1, 1, 2);
    remote.scope.predicate.count = 1;
    remote.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_CHANNEL, 0};
    uint8_t data[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(&remote, data);
    CHECK(core_endpoint_submit(&owner, 3, data, size, 100, &record) == CORE_ADMIT_UNAUTHORIZED);
    CHECK(core_endpoint_submit(&owner, 1, data, size, 100, &record) == CORE_ADMIT_CONFLICT);
    CHECK(core_admission_establish_node(&owner, (core_instance_id){1}));
    CHECK(core_endpoint_submit(&owner, 1, data, size, 100, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->endpoint_only && record->node == NULL && !record->request_present);
    CHECK(record->deadline_us == 0 && record->scope.coordinator == 1);
    CHECK(owner.reserved_bytes == 5120);
    CHECK(core_endpoint_submit(&owner, 1, data, size, UINT64_MAX, &record) == CORE_ADMIT_DUPLICATE);
    core_access token;
    CHECK(core_action_issue(&owner.endpoints[0].prepare, &owner.endpoints[0].lifetime, &token));
    CHECK(core_endpoint_cancel(&owner, 1, data, size, 101, &record) == CORE_ADMIT_DUPLICATE);
    CHECK(record->cleanup_selected && owner.endpoints[0].lifetime.closing);
    CHECK(!core_action_begin(&owner.endpoints[0].prepare, token));
    CHECK(!core_admission_release_endpoints(&owner, (core_op_id){2}));
    core_work_result result;
    CHECK(core_action_accept(&owner.endpoints[0].prepare, token, &result));
    CHECK(result == CORE_WORK_CANCELLED);
    CHECK(core_admission_release_endpoints(&owner, (core_op_id){2}));
    CHECK(!owner.nodes[0].lifetime.closing);
    CHECK(core_endpoint_submit(&owner, 1, data, size, 102, &record) == CORE_ADMIT_DUPLICATE);

    remote.id.value = 3;
    remote.scope.bindings[0].channel.source_instance.value = 3;
    const size_t fresh_size = fixture_encode(&remote, data);
    CHECK(core_endpoint_cancel(&owner, 1, data, fresh_size, 103, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->cleanup_selected && owner.reserved_bytes == 4096);
    CHECK(core_endpoint_submit(&owner, 1, data, fresh_size, 104, &record) == CORE_ADMIT_DUPLICATE);
    CHECK(!owner.endpoints[0].occupied);
}

static void byte_transaction_and_reuse(void)
{
    core_admission pool;
    CHECK(core_admission_init(&pool, 128, 128));
    const uint64_t peers[] = {1, 2};
    CHECK(core_admission_configure(&pool, 1, 99, peers, 2));
    fixture_request add = fixture_node(0, 0);
    add.scope.kind = CORE_REQUEST_ADD_NODE;
    add.scope.channel_count = 1;
    add.scope.bindings[0].channel = fixture_channel(0, 20, 0, 2, 1, 2);
    add.scope.predicate.count = 1;
    add.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_TRUE, 0};
    const core_admission_record *record = NULL;
    const core_admission before = pool;
    CHECK(fixture_submit(&pool, &add, &record) == CORE_ADMIT_FULL);
    CHECK(record == NULL && memcmp(&before, &pool, sizeof(pool)) == 0);
    add.scope.kind = CORE_REQUEST_ADD_NODE_NO_EDGES;
    CHECK(fixture_submit(&pool, &add, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->id.value == 0 && record->node->instance.value == 0);
    core_access old, fresh;
    CHECK(core_access_acquire(&record->node->lifetime, &old));
    CHECK(core_admission_close_scope(&pool, (core_op_id){0}));
    CHECK(!core_admission_release_node(&pool, (core_op_id){0}));
    CHECK(core_access_release(&record->node->lifetime, old));
    CHECK(core_admission_release_node(&pool, (core_op_id){0}));
    CHECK(core_admission_forget(&pool, (core_op_id){0}));
    add.id.value = 4;
    CHECK(fixture_submit(&pool, &add, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_access_acquire(&record->node->lifetime, &fresh));
    CHECK(old.owner == fresh.owner && old.ticket != fresh.ticket);
    CHECK(!core_access_release(&record->node->lifetime, old));
    CHECK(core_access_release(&record->node->lifetime, fresh));
}

static void maximum_endpoint_profile(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const core_admission_record *record;
    fixture_request first = fixture_node(1, 10);
    CHECK(fixture_submit(&pool, &first, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){1}));
    fixture_request second = fixture_node(2, 20);
    second.scope.kind = CORE_REQUEST_ADD_NODE;
    second.scope.channel_count = CORE_MAX_REQUEST_CHANNELS;
    second.scope.port_count = CORE_MAX_PORTS;
    second.scope.ports[2] = (core_port){.id = 3, .direction = CORE_PORT_INPUT,
                                     .type = 3, .max_payload = CORE_MAX_PAYLOAD_BYTES};
    second.scope.ports[3] = (core_port){.id = 4, .direction = CORE_PORT_OUTPUT,
                                     .type = 3, .max_payload = CORE_MAX_PAYLOAD_BYTES};
    second.scope.predicate.count = 1;
    second.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_TRUE, 0};
    for (size_t i = 0; i < CORE_MAX_REQUEST_CHANNELS; ++i) {
        second.scope.bindings[i].channel = fixture_channel(10, 20, 1, 2, 1, 1);
        second.scope.bindings[i].channel.service += i;
    }
    CHECK(fixture_submit(&pool, &second, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->scope.object_count == CORE_REPORT_MAX_OBJECTS);
    CHECK(core_report_revision_bound(record->scope.object_count) == 78);
    CHECK(pool.reserved_bytes == pool.byte_limit);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){2}));
    fixture_request edge = {.id = {3}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    edge.scope.bindings[0].channel = fixture_channel(10, 20, 1, 2, 1, 1);
    edge.scope.bindings[0].channel.service = 11;
    const core_admission before = pool;
    CHECK(fixture_submit(&pool, &edge, &record) == CORE_ADMIT_FULL);
    CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);
    fixture_request remove = {.id = {3}, .scope = {.kind = CORE_REQUEST_REMOVE_EDGE,
        .channel_count = 1, .allowance_us = 50}};
    remove.scope.bindings[0] = (core_binding){second.scope.bindings[0].channel, {2}};
    CHECK(fixture_submit(&pool, &remove, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_close_scope(&pool, (core_op_id){3}));
    CHECK(!core_admission_release_endpoints(&pool, (core_op_id){2}));
    CHECK(core_admission_release_binding(&pool, &remove.scope.bindings[0]));
    CHECK(pool.reserved_bytes == pool.byte_limit - 2 * CORE_ENDPOINT_BYTES);
    CHECK(!pool.endpoints[2].lifetime.closing && !pool.nodes[0].lifetime.closing);
    CHECK(!pool.nodes[1].lifetime.closing);
    CHECK(core_admission_close_scope(&pool, (core_op_id){2}));
    CHECK(core_admission_release_endpoints(&pool, (core_op_id){2}));
    CHECK(core_admission_release_node(&pool, (core_op_id){2}));
    CHECK(pool.reserved_bytes == 4096 && !pool.nodes[0].lifetime.closing);
}

static void distinct_instances_of_same_logical_node(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const core_admission_record *record;
    fixture_request first = fixture_node(1, 10);
    fixture_request second = fixture_node(2, 10);
    CHECK(fixture_submit(&pool, &first, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(fixture_submit(&pool, &second, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(pool.nodes[0].logical_node_id == 10 && pool.nodes[1].logical_node_id == 10);
    CHECK(pool.nodes[0].instance.value == 1 && pool.nodes[1].instance.value == 2);
    CHECK(pool.reserved_bytes == 8192);
    fixture_request remove = {.id = {3}, .scope = {.kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES,
        .logical_node_id = 10, .instance = {1}, .allowance_us = 50}};
    CHECK(fixture_submit(&pool, &remove, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_close_scope(&pool, (core_op_id){3}));
    CHECK(core_admission_release_node(&pool, (core_op_id){1}));
    CHECK(!pool.nodes[0].occupied && pool.nodes[1].occupied && !pool.nodes[1].lifetime.closing);
    CHECK(pool.reserved_bytes == 4096);
}

static fixture_request node_with_remote_incidence(uint8_t count)
{
    fixture_request add = fixture_node(1, 10);
    add.scope.kind = CORE_REQUEST_ADD_NODE;
    add.scope.channel_count = count;
    add.scope.predicate.count = 1;
    add.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_TRUE, 0};
    for (uint8_t i = 0; i < count; ++i) {
        add.scope.bindings[i].channel = fixture_channel(10, 20 + i, 1, 10 + i, 1, 2);
        add.scope.bindings[i].channel.service += i;
    }
    return add;
}

static void complete_incidence_bound(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const core_admission_record *record;
    fixture_request add = node_with_remote_incidence(CORE_MAX_REQUEST_CHANNELS);
    CHECK(fixture_submit(&pool, &add, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){1}));
    CHECK(pool.reserved_bytes == 4096 + 4 * CORE_ENDPOINT_BYTES);
    fixture_request edge = {.id = {2}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    edge.scope.bindings[0].channel = fixture_channel(10, 30, 1, 20, 1, 2);
    edge.scope.bindings[0].channel.service = 50;
    core_admission before = pool;
    const core_admission_record *unchanged = record;
    CHECK(fixture_submit(&pool, &edge, &record) == CORE_ADMIT_FULL);
    CHECK(record == unchanged && memcmp(&pool, &before, sizeof(pool)) == 0);
    CHECK(!pool.endpoints[4].occupied && !pool.records[1].occupied);
    fixture_request incoming = edge;
    incoming.id.value = 3;
    incoming.scope.bindings[0].channel = fixture_channel(31, 10, 21, 1, 2, 1);
    uint8_t wire[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(&incoming, wire);
    CHECK(core_endpoint_submit(&pool, 2, wire, size, 10, &record) == CORE_ADMIT_FULL);
    CHECK(record == unchanged && memcmp(&pool, &before, sizeof(pool)) == 0);
    /* Selection/closure without release still occupies incidence capacity. */
    core_lifetime_close(&pool.endpoints[0].lifetime);
    before = pool;
    CHECK(fixture_submit(&pool, &edge, &record) == CORE_ADMIT_FULL);
    CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);
    core_binding released = {.channel = add.scope.bindings[0].channel, .creating_op = {1}};
    CHECK(core_admission_release_binding(&pool, &released));
    /* Reusing incidence also requires accepted closure of the remote role. */
    CHECK(core_report_observe(&pool.records[0].evidence, 2,
        CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED) == CORE_REPORT_CHANGED);
    CHECK(fixture_submit(&pool, &edge, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(pool.reserved_bytes == 4096 + 4 * CORE_ENDPOINT_BYTES);
}

static void new_node_batch_preserves_neighbor_incidence_bound(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const core_admission_record *record;
    fixture_request first = node_with_remote_incidence(3);
    CHECK(fixture_submit(&pool, &first, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){1}));
    fixture_request second = fixture_node(2, 30);
    second.scope.kind = CORE_REQUEST_ADD_NODE;
    second.scope.channel_count = 2;
    second.scope.predicate.count = 1;
    second.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_TRUE, 0};
    for (uint8_t i = 0; i < 2; ++i) {
        second.scope.bindings[i].channel = fixture_channel(10, 30, 1, 2, 1, 1);
        second.scope.bindings[i].channel.service = 50 + i;
    }
    const core_admission before = pool;
    const core_admission_record *unchanged = record;
    CHECK(fixture_submit(&pool, &second, &record) == CORE_ADMIT_FULL);
    CHECK(record == unchanged && memcmp(&pool, &before, sizeof(pool)) == 0);
    CHECK(!pool.nodes[1].occupied && !pool.endpoints[3].occupied);
    /* Three existing bindings plus one colocated binding fits. Its two local
     * endpoint roles must contribute only one to either instance's incidence. */
    second.scope.channel_count = 1;
    CHECK(fixture_submit(&pool, &second, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(pool.reserved_bytes == 8192 + 5 * CORE_ENDPOINT_BYTES);
}

int main(void)
{
    all_six_and_cleanup();
    precreation_fences_and_capacity();
    remote_reservations_and_cancellation();
    byte_transaction_and_reuse();
    maximum_endpoint_profile();
    distinct_instances_of_same_logical_node();
    complete_incidence_bound();
    new_node_batch_preserves_neighbor_incidence_bound();
    puts("admission: all six variants, atomic capacities, incidence, scoped cleanup and remote fences passed");
    return 0;
}

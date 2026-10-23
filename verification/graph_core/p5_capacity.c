/* Reuse the unchanged P4 qualification's bounded guest/data/memory helpers.
 * This new scenario constructs the final node with eight initial channels. */
#define main p4_fixture_main
#include "p4_capacity.c"
#undef main

static core_binding initial_bindings[CHANNELS];

static bool entry_guard(void *context, size_t slot)
{
    (void)context;
    pthread_mutex_lock(&registry);
    const uint64_t now = now_us();
    core_edge_step(&fixture.edges, now);
    core_node_step(&fixture.nodes, now);
    return core_node_start_allowed(&fixture.nodes, slot, now);
}

static void entry_release(void *context)
{
    (void)context;
    pthread_mutex_unlock(&registry);
}

static core_channel ring_channel(size_t n, size_t c)
{
    const size_t next = (n + 1) % NODES;
    core_channel channel = fixture_channel(100 + n, 100 + next,
        fixture.node_ids[n].value, fixture.node_ids[next].value, 1, 1);
    channel.source_port = (uint16_t)(c + 1);
    channel.destination_port = (uint16_t)(c + 5);
    channel.service = 7 + c;
    channel.payload_bytes = CORE_MAX_PAYLOAD_BYTES;
    channel.queue_messages = CORE_MAX_QUEUE_MESSAGES;
    channel.addition_allowance_us = ALLOWANCE_US;
    return channel;
}

static void construct(const uint8_t *artifact, size_t artifact_size)
{
    for (size_t n = 0; n < NODES; ++n) fixture.node_ids[n] = next_id();
    for (size_t n = 0; n < NODES; ++n) {
        fixture_request request = fixture_node(fixture.node_ids[n].value, 100 + n);
        request.node.artifact_bytes = (uint32_t)artifact_size;
        request.node.required_bytes = (uint32_t)fixture.admission.node_bytes;
        request.node.addition_allowance_us = request.node.cleanup_allowance_us = ALLOWANCE_US;
        request.node.configuration_size = 24;
        encode64(request.node.configuration, 100 + n);
        encode64(request.node.configuration + 8, 100 + (n + NODES - 1) % NODES);
        encode64(request.node.configuration + 16, 100 + (n + 1) % NODES);
        request.scope.port_count = 8;
        for (size_t p = 0; p < 8; ++p)
            request.scope.ports[p] = (core_port){.id = (uint16_t)(p + 1), .type = 3,
                .direction = p < 4 ? CORE_PORT_OUTPUT : CORE_PORT_INPUT,
                .max_payload = CORE_MAX_PAYLOAD_BYTES};
        if (n == NODES - 1) {
            request.scope.kind = CORE_REQUEST_ADD_NODE;
            request.scope.channel_count = 8;
            /* A required input and output; six optional initial channels. */
            const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0},
                {CORE_PREDICATE_CHANNEL, 4}, {CORE_PREDICATE_AND, 0}};
            CHECK(core_predicate_init(&request.scope.predicate, tokens, 3, 8));
            for (size_t c = 0; c < 8; ++c) {
                core_binding *b = &request.scope.bindings[c];
                b->channel = ring_channel(c < 4 ? 2 : 3, c % 4);
                b->creating_op = request.id;
                initial_bindings[8 + c] = *b;
            }
        }
        uint8_t *copy = malloc(artifact_size);
        CHECK(copy != NULL);
        memcpy(copy, artifact, artifact_size);
        pthread_mutex_lock(&registry);
        const core_admission_record *record = submit(&request);
        CHECK(core_lifetime_charge(&record->node->lifetime, artifact_size));
        CHECK(core_node_artifact(&fixture.nodes, record->id, copy, artifact_size, now_us()));
        pthread_mutex_unlock(&registry);
        await_success(request.id);
    }
    for (size_t n = 0; n < 2; ++n) {
        for (size_t c = 0; c < 4; ++c) {
            fixture_request request = {.id = next_id(), .scope = {.kind = CORE_REQUEST_ADD_EDGE,
                .channel_count = 1, .cleanup_allowance_us = ALLOWANCE_US}};
            request.scope.bindings[0] = (core_binding){.channel = ring_channel(n, c),
                                                       .creating_op = request.id};
            initial_bindings[4 * n + c] = request.scope.bindings[0];
            pthread_mutex_lock(&registry);
            (void)submit(&request);
            pthread_mutex_unlock(&registry);
            await_success(request.id);
        }
    }
}

static void reclaim(void)
{
    for (size_t c = 0; c < CHANNELS; ++c) {
        fixture_request request = {.id = next_id(), .scope = {.kind = CORE_REQUEST_REMOVE_EDGE,
            .channel_count = 1, .allowance_us = ALLOWANCE_US}};
        request.scope.bindings[0] = initial_bindings[c];
        pthread_mutex_lock(&registry);
        (void)submit(&request);
        pthread_mutex_unlock(&registry);
        await_success(request.id);
        pthread_mutex_lock(&registry);
        if (c < 8) CHECK(core_admission_forget(&fixture.admission, initial_bindings[c].creating_op));
        CHECK(core_admission_forget(&fixture.admission, request.id));
        CHECK(core_report_outcome(&core_admission_find(&fixture.admission,
            fixture.node_ids[3])->evidence) == CORE_OUTCOME_SUCCEEDED);
        pthread_mutex_unlock(&registry);
    }
    for (size_t n = 0; n < NODES; ++n) {
        fixture_request request = {.id = next_id(), .scope = {.kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES,
            .logical_node_id = 100 + n, .instance = {fixture.node_ids[n].value}, .allowance_us = ALLOWANCE_US}};
        pthread_mutex_lock(&registry);
        CHECK(fixture.admission.nodes[n].established);
        (void)submit(&request);
        pthread_mutex_unlock(&registry);
        await_success(request.id);
        pthread_mutex_lock(&registry);
        CHECK(core_admission_forget(&fixture.admission, fixture.node_ids[n]));
        CHECK(core_admission_forget(&fixture.admission, request.id));
        pthread_mutex_unlock(&registry);
    }
    CHECK(fixture.admission.reserved_bytes == 0);
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e)
        CHECK(!fixture.admission.endpoints[e].occupied &&
              core_lifetime_reclaimed(&fixture.admission.endpoints[e].lifetime));
    for (size_t n = 0; n < NODES; ++n) {
        CHECK(!fixture.admission.nodes[n].occupied &&
              core_lifetime_reclaimed(&fixture.admission.nodes[n].lifetime));
        for (size_t c = 0; c < CORE_MAX_REQUEST_CHANNELS; ++c)
            CHECK(!fixture.admission.nodes[n].notifications[c].occupied);
    }
}

void p5_capacity_run(const uint8_t *artifact, size_t artifact_size)
{
    CHECK(core_wamr_profile(artifact, artifact_size));
#ifdef ESP_PLATFORM
    changed = xSemaphoreCreateBinaryStatic(&wake_storage);
    CHECK(changed != NULL);
    memory_snapshot("p5_before_runtime");
#endif
    CHECK(core_admission_init(&fixture.admission, artifact_size + P4_CAPACITY_ARENA_BYTES,
        NODES * (artifact_size + P4_CAPACITY_ARENA_BYTES) + CORE_MAX_ENDPOINT_RESERVATIONS * CORE_ENDPOINT_BYTES));
    const uint64_t peers[] = {1};
    CHECK(core_admission_configure(&fixture.admission, 1, 99, peers, 1));
    core_endpoint_init(&fixture.endpoints, &fixture.admission, channel_changed, NULL);
    fixture.edges.admission = &fixture.admission;
    fixture.edges.endpoints = &fixture.endpoints;
    fixture.nodes.admission = &fixture.admission;
    CHECK(core_wamr_init(&fixture.nodes.runtime, wake_control, NULL));
    const core_wamr_channels callbacks = {.put = guest_put, .get = guest_get,
        .event_get = guest_event, .event_ready = guest_event_ready};
    CHECK(core_wamr_attach_channels(&callbacks));
    CHECK(core_wamr_attach_entry_guard(NULL, entry_guard, entry_release));
#ifdef ESP_PLATFORM
    size_t reclaimed_baseline = 0;
#endif
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        pthread_mutex_lock(&registry);
        fixture.hold_reads = fixture.hold_transfer = true;
        memset(fixture.puts, 0, sizeof(fixture.puts));
        memset(fixture.gets, 0, sizeof(fixture.gets));
        memset(fixture.full, 0, sizeof(fixture.full));
        memset(fixture.validated, 0, sizeof(fixture.validated));
        fixture.deliveries = 0;
#ifdef ESP_PLATFORM
        for (size_t n = 0; n < NODES; ++n) fixture.worker_stack_free[n] = SIZE_MAX;
#endif
        pthread_mutex_unlock(&registry);
        construct(artifact, artifact_size);
        fill_all_queues();
        exchange_traffic();
        reclaim();
        printf("P5 capacity cycle=%u reservations=0\n", cycle);
#ifdef ESP_PLATFORM
        memory_snapshot("p5_reclaimed");
        const size_t reclaimed = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (cycle == 0) reclaimed_baseline = reclaimed;
        else CHECK(reclaimed == reclaimed_baseline);
        for (size_t n = 0; n < NODES; ++n) {
            CHECK(fixture.worker_task[n] != NULL);
            CHECK(uxTaskGetStackHighWaterMark(fixture.worker_task[n]) >= 2048);
        }
#endif
    }
    core_wamr_shutdown();
    puts("P5 capacity PASS: four WAMR nodes, eight initial channels, full queues and reclamation");
}

#ifndef ESP_PLATFORM
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    CHECK(length > 0 && length <= CORE_MAX_ARTIFACT_BYTES && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *artifact = malloc((size_t)length);
    CHECK(artifact != NULL && fread(artifact, 1, (size_t)length, file) == (size_t)length);
    CHECK(fclose(file) == 0);
    p5_capacity_run(artifact, (size_t)length);
    free(artifact);
    return 0;
}
#endif

/* Reuse the unchanged four-node WAMR workload. Exercise report retirement
 * across complete lifecycles without manual record-forget calls. */
#define main p4_fixture_main
#include "p4_capacity.c"
#undef main

static void full_removal(void)
{
    core_op_id removals[NODES];
    pthread_mutex_lock(&registry);
    /* Admit four overlapping eight-binding removals at the full record bound.
     * All queues are occupied and guests may still own runtime accesses. */
    for (size_t n = 0; n < NODES; ++n) {
        fixture_request request = {.id = next_id(), .scope = {
            .kind = CORE_REQUEST_REMOVE_NODE, .logical_node_id = 100 + n,
            .instance = {fixture.node_ids[n].value}, .allowance_us = ALLOWANCE_US}};
        for (size_t c = 0; c < CHANNELS; ++c) {
            const core_admission_record *r = core_admission_find(&fixture.admission,
                                                                fixture.edge_ids[c]);
            const core_binding *b = &r->scope.bindings[0];
            if (b->channel.source_instance.value == request.scope.instance.value ||
                b->channel.destination_instance.value == request.scope.instance.value)
                request.scope.bindings[request.scope.channel_count++] = *b;
        }
        CHECK(request.scope.channel_count == 8);
        (void)submit(&request);
        removals[n] = request.id;
    }
    pthread_mutex_unlock(&registry);
    for (size_t n = 0; n < NODES; ++n) await_success(removals[n]);
    pthread_mutex_lock(&registry);
    CHECK(fixture.admission.reserved_bytes == 0);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        const core_admission_record *r = &fixture.admission.records[i];
        CHECK(r->occupied && core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED);
        CHECK(core_report_obligations(&r->evidence) == 0);
        if (r->scope.kind == CORE_REQUEST_REMOVE_NODE) {
            CHECK(r->scope.object_count == 17);
            for (size_t o = 0; o < 17; ++o) CHECK((r->evidence.objects[o] & 56) == 56);
        }
    }
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i)
        CHECK(!fixture.admission.endpoints[i].occupied && fixture.endpoints.queues[i].count == 0);
    core_admission_maintain(&fixture.admission);
    /* Retire only the removal reports. Creator reports changed during cleanup,
     * so their earlier storage ACKs must leave final versions retrievable. */
    for (size_t n = 0; n < NODES; ++n) {
        const core_admission_record *r = core_admission_find(&fixture.admission, removals[n]);
        core_admission_acknowledge(&fixture.admission, r->id, 1, r->evidence.revision);
    }
    core_admission_maintain(&fixture.admission);
    for (size_t n = 0; n < NODES; ++n)
        CHECK(core_admission_find(&fixture.admission, removals[n]) == NULL);
    for (size_t n = 0; n < NODES; ++n)
        CHECK(core_admission_report_retained(core_admission_find(&fixture.admission, fixture.node_ids[n])));
    /* Next cycle's successful admissions evict eligible unacknowledged history. */
    pthread_mutex_unlock(&registry);
}

void p7_capacity_run(const uint8_t *artifact, size_t size)
{
    CHECK(core_wamr_profile(artifact, size));
#ifdef ESP_PLATFORM
    changed = xSemaphoreCreateBinaryStatic(&wake_storage);
    CHECK(changed != NULL);
#endif
    CHECK(core_admission_init(&fixture.admission, size + P4_CAPACITY_ARENA_BYTES,
        NODES * (size + P4_CAPACITY_ARENA_BYTES) + CORE_MAX_ENDPOINT_RESERVATIONS * CORE_ENDPOINT_BYTES));
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
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        pthread_mutex_lock(&registry);
        fixture.hold_reads = fixture.hold_transfer = true;
        memset(fixture.puts, 0, sizeof(fixture.puts));
        memset(fixture.gets, 0, sizeof(fixture.gets));
        memset(fixture.full, 0, sizeof(fixture.full));
        memset(fixture.validated, 0, sizeof(fixture.validated));
        fixture.deliveries = 0;
        pthread_mutex_unlock(&registry);
        add_nodes(artifact, size);
        add_edges();
        fill_all_queues();
        pthread_mutex_lock(&registry);
        for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
            const core_admission_record *r = &fixture.admission.records[i];
            if (r->occupied) core_admission_acknowledge(&fixture.admission, r->id, 1, r->evidence.revision);
        }
        core_admission_maintain(&fixture.admission);
        for (size_t n = 0; n < NODES; ++n) {
            const core_admission_record *r = core_admission_find(&fixture.admission, fixture.node_ids[n]);
            CHECK(r != NULL && r->node != NULL && !core_admission_report_retained(r));
        }
        pthread_mutex_unlock(&registry);
        full_removal();
        printf("P7 capacity cycle=%u reservations=0\n", cycle);
#ifdef ESP_PLATFORM
        memory_snapshot("p7_reclaimed");
#endif
    }
    pthread_mutex_lock(&registry);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        const core_admission_record *r = &fixture.admission.records[i];
        if (r->occupied) core_admission_acknowledge(&fixture.admission, r->id, 1, r->evidence.revision);
    }
    core_admission_maintain(&fixture.admission);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) CHECK(!fixture.admission.records[i].occupied);
    pthread_mutex_unlock(&registry);
    core_wamr_shutdown();
    puts("P7 capacity PASS: live ACKs, later evidence, pressure eviction and full reclamation");
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
    p7_capacity_run(artifact, (size_t)length);
    free(artifact);
    return 0;
}
#endif

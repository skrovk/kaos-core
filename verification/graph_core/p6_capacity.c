/* Existing workload helpers remain unchanged; only this removal slice is new. */
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
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i)
        CHECK(core_admission_forget(&fixture.admission, fixture.admission.records[i].id));
    pthread_mutex_unlock(&registry);
}

void p6_capacity_run(const uint8_t *artifact, size_t size)
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
        full_removal();
        printf("P6 capacity cycle=%u reservations=0\n", cycle);
#ifdef ESP_PLATFORM
        memory_snapshot("p6_reclaimed");
#endif
    }
    core_wamr_shutdown();
    puts("P6 capacity PASS: four WAMR nodes, overlapping full removals and occupied queues");
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
    p6_capacity_run(artifact, (size_t)length);
    free(artifact);
    return 0;
}
#endif

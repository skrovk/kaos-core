/* Use existing deterministic runtime/transport helpers without changing cases. */
#define main p5_fixture_main
#include "p5_test.c"
#undef main

static const core_admission_record *remove_all(fixture *f, uint64_t id, uint64_t allowance)
{
    fixture_request r = {.id = {id}, .scope = {.kind = CORE_REQUEST_REMOVE_NODE,
        .logical_node_id = 10, .instance = {100}, .allowance_us = allowance,
        .channel_count = f->record->scope.channel_count}};
    for (size_t c = 0; c < r.scope.channel_count; ++c) r.scope.bindings[c] = f->record->scope.bindings[c];
    const core_admission_record *out;
    CHECK(fixture_submit(&f->devices[0].admission, &r, &out) == CORE_ADMIT_ACCEPTED);
    return out;
}

static void optional_progress_after_storage_ack(void)
{
    fixture f;
    const core_predicate_token yes[] = {{CORE_PREDICATE_TRUE, 0}};
    const fixture_request request = request_for(1, 0, yes, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, (delivery){0, 0, 0, false, false});
    marker(&f, 13);
    success(&f);
    core_admission *pool = &f.devices[0].admission;
    const uint8_t revision = f.record->evidence.revision;
    const uint64_t deadline = f.record->deadline_us;
    core_admission_acknowledge(pool, f.record->id, 1, revision);
    core_admission_maintain(pool);
    CHECK(!core_admission_report_retained(f.record));
    CHECK(f.record->node != NULL && runtime(&f)->main_active);
    pump(&f, 20, all);
    CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == 1);
    CHECK(f.record->evidence.revision > revision && f.record->deadline_us == deadline);
    core_admission_acknowledge(pool, f.record->id, 1, revision);
    core_admission_maintain(pool);
    CHECK(core_admission_report_retained(f.record));
    success(&f);
    CHECK(f.devices[0].runtime.prepares == 1);
}

static void unresolved_optional_survives_ack_and_pressure(void)
{
    fixture f;
    const core_predicate_token yes[] = {{CORE_PREDICATE_TRUE, 0}};
    const fixture_request request = request_for(1, 0, yes, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, (delivery){0, 0, 0, false, false});
    marker(&f, 13);
    pump(&f, 210, (delivery){0, 0, 0, false, false});
    pump(&f, 260, (delivery){0, 0, 0, false, false});
    success(&f);
    CHECK(core_report_obligations(&f.record->evidence) != 0);
    core_admission *pool = &f.devices[0].admission;
    core_admission_acknowledge(pool, f.record->id, 1, f.record->evidence.revision);
    core_admission_maintain(pool);
    for (size_t i = 1; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        const fixture_request removal = {.id = {200 + i}, .scope = {
            .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 90,
            .instance = {900}, .allowance_us = 50}};
        const core_admission_record *out;
        CHECK(fixture_submit(pool, &removal, &out) == CORE_ADMIT_ACCEPTED);
    }
    const core_admission before = *pool;
    const fixture_request extra = {.id = {999}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 90,
        .instance = {900}, .allowance_us = 50}};
    const core_admission_record *out = NULL;
    CHECK(fixture_submit(pool, &extra, &out) == CORE_ADMIT_FULL);
    CHECK(memcmp(&before, pool, sizeof(before)) == 0);
}

static void retired_observer_preserves_closure_and_history(void)
{
    fixture f;
    const core_predicate_token yes[] = {{CORE_PREDICATE_TRUE, 0}};
    const fixture_request request = request_for(1, 0, yes, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, all);
    marker(&f, 13);
    const core_admission_record *removal = remove_all(&f, 200, 100);
    pump(&f, 20, all);
    dispose(&f, 21);
    CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(f.record->node == NULL);
    const core_report original = f.record->evidence;
    core_admission *pool = &f.devices[0].admission;
    core_admission_acknowledge(pool, removal->id, 1, removal->evidence.revision);
    core_admission_maintain(pool);
    CHECK(core_admission_find(pool, (core_op_id){200}) == NULL);
    CHECK(memcmp(&original, &f.record->evidence, sizeof(original)) == 0);
    CHECK(f.record->closed_objects & (UINT32_C(1) << 2));
    /* No remote incidence remains merely because its observer was retired. */
    const fixture_request no_edges = {.id = {201}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 10,
        .instance = {100}, .allowance_us = 100}};
    const core_admission_record *out;
    CHECK(fixture_submit(pool, &no_edges, &out) == CORE_ADMIT_ACCEPTED);
    core_admission_acknowledge(pool, f.record->id, 1, f.record->evidence.revision);
    core_admission_maintain(pool);
    CHECK(core_admission_find(pool, (core_op_id){100}) == NULL);
}

static void retire_owner_under_pressure(device *d, core_op_id id)
{
    size_t count = 0;
    while (core_admission_find(&d->admission, id) != NULL) {
        CHECK(count < CORE_MAX_ADMISSION_RECORDS);
        const fixture_request filler = {.id = {1000 + count}, .scope = {
            .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 90,
            .instance = {900}, .allowance_us = 1000}};
        const core_admission_record *out;
        CHECK(fixture_submit(&d->admission, &filler, &out) == CORE_ADMIT_ACCEPTED);
        ++count;
    }
    /* Settle the unrelated pressure requests through their actual service,
     * then release their reports so the replay below has room to be admitted. */
    core_node_step(&d->nodes, 22);
    for (size_t i = 0; i < count; ++i) {
        const core_admission_record *r = core_admission_find(&d->admission, (core_op_id){1000 + i});
        CHECK(r != NULL && core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED);
        core_admission_acknowledge(&d->admission, r->id, d->admission.local_device, r->evidence.revision);
    }
    core_admission_maintain(&d->admission);
}

static void owner_delivery_and_same_id_slot_reuse(void)
{
    fixture f;
    const core_predicate_token yes[] = {{CORE_PREDICATE_TRUE, 0}};
    const fixture_request request = request_for(1, 0, yes, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, all);
    marker(&f, 13);
    remove_all(&f, 200, 100);
    step(&f.devices[0], 20);
    core_edge_effect command;
    CHECK(core_edge_next(&f.devices[0].edges, 2, 20, &command));
    CHECK(command.operation.value == 200);
    const message issued = {.effect = command, .from = 0, .to = 1, .occupied = true};
    deliver(&f, &issued, 20);
    device *peer = &f.devices[1];
    step(peer, 21);
    core_edge_effect final = {0};
    while (core_edge_next(&peer->edges, 1, 21, &final)) {
        core_owner_report report;
        CHECK(core_owner_report_decode(final.body, final.size, &report));
        CHECK(report.facts & CORE_OBJECT_CLEANED);
        const core_admission_record *r = core_admission_find(&peer->admission, final.operation);
        core_admission_maintain(&peer->admission);
        CHECK(core_admission_find(&peer->admission, final.operation) == r);
        const message response = {.effect = final, .from = 1, .to = 0, .occupied = true};
        deliver(&f, &response, 21);
        core_edge_delivered(&peer->edges, &final);
        core_admission_maintain(&peer->admission);
        CHECK(core_admission_find(&peer->admission, final.operation) == r);
        CHECK(r->owner_delivered != 0);
        if (final.operation.value == 200) break;
    }
    CHECK(final.operation.value == 200);
    retire_owner_under_pressure(peer, final.operation);
    /* Q05 allows the retired command to create another owner record. Internal
     * old callback/effect lifetimes must still never alias this replacement. */
    const core_admission_record *replacement;
    CHECK(core_endpoint_removal_submit(&peer->admission, 1, command.body,
        command.size, &replacement) == CORE_ADMIT_ACCEPTED);
    CHECK(replacement->incarnation != final.incarnation);
    CHECK(!core_edge_effect_current(&peer->edges, &final, 22));
    core_edge_delivered(&peer->edges, &final);
    CHECK(replacement->owner_delivered == 0);
    step(peer, 22);
    core_edge_effect again;
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        CHECK(core_edge_next(&peer->edges, 1, 22, &again));
        if (again.operation.value == 200) break;
    }
    CHECK(again.operation.value == 200 && again.incarnation == replacement->incarnation);
    CHECK(replacement->owner_delivered == 0);
    puts("Q05 characterization: retired removal command recreated its owner record; old callback rejected");
}

static uint8_t owner_exchange(device *d, const core_edge_effect *effect, uint16_t mid, uint64_t now)
{
    const uint8_t format = 60;
    const core_coap_message request = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = mid, .token_size = 1, .token = {42}, .option_count = 2,
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)"owner", 5},
                    {CORE_COAP_CONTENT_FORMAT, &format, 1}},
        .payload = effect->body, .payload_size = effect->size};
    uint8_t packet[CORE_COAP_MAX_DATAGRAM], response[CORE_COAP_MAX_DATAGRAM];
    size_t size, response_size;
    CHECK(core_coap_encode(&request, packet, sizeof(packet), &size));
    CHECK(core_control_receive(&d->control, 2, packet, size, now,
        response, sizeof(response), &response_size));
    core_coap_message reply;
    CHECK(core_coap_decode(response, response_size, &reply));
    CHECK(reply.payload_size == 0);
    return reply.code;
}

static void lost_reply_survives_retirement_and_reordered_duplicates(void)
{
    fixture f;
    const core_predicate_token yes[] = {{CORE_PREDICATE_TRUE, 0}};
    const fixture_request request = request_for(1, 0, yes, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, all);
    marker(&f, 13);
    const core_admission_record *removal = remove_all(&f, 200, 100);
    step(&f.devices[0], 20);
    core_edge_effect command;
    CHECK(core_edge_next(&f.devices[0].edges, 2, 20, &command));
    const message m = {.effect = command, .from = 0, .to = 1, .occupied = true};
    deliver(&f, &m, 20);
    step(&f.devices[1], 21);
    core_edge_effect reports[2];
    for (size_t i = 0; i < 2; ++i) {
        core_edge_effect effect;
        CHECK(core_edge_next(&f.devices[1].edges, 1, 21, &effect));
        CHECK(effect.operation.value == 100 || effect.operation.value == 200);
        reports[effect.operation.value == 200] = effect;
    }
    CHECK(owner_exchange(&f.devices[0], &reports[0], 60000, 21) == CORE_COAP_CHANGED);
    CHECK(owner_exchange(&f.devices[0], &reports[1], 60001, 21) == CORE_COAP_CHANGED);
    /* An older duplicate arrives while its original record is still retained. */
    CHECK(owner_exchange(&f.devices[0], &reports[0], 60000, 22) == CORE_COAP_CHANGED);
    runtime(&f)->disposed = true;
    runtime(&f)->main_active = false;
    step(&f.devices[0], 22);
    CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    core_admission_acknowledge(&f.devices[0].admission, removal->id, 1, removal->evidence.revision);
    core_admission_maintain(&f.devices[0].admission);
    CHECK(core_admission_find(&f.devices[0].admission, (core_op_id){200}) == NULL);
    const core_admission before = f.devices[0].admission;
    /* Ki retries the same exchange after the first successful reply was lost. */
    CHECK(owner_exchange(&f.devices[0], &reports[1], 60001, 23) == CORE_COAP_CHANGED);
    CHECK(memcmp(&before, &f.devices[0].admission, sizeof(before)) == 0);
    core_edge_delivered(&f.devices[1].edges, &reports[1]);
    core_admission_maintain(&f.devices[1].admission);
    const core_admission_record *owner = core_admission_find(&f.devices[1].admission, (core_op_id){200});
    CHECK(owner != NULL && owner->owner_delivered != 0);
    /* Same transport key with different evidence is never acknowledged. */
    core_edge_effect conflict = reports[1];
    core_owner_report decoded;
    CHECK(core_owner_report_decode(conflict.body, conflict.size, &decoded));
    decoded.facts |= CORE_OBJECT_FAILED;
    CHECK(core_owner_report_encode(&decoded, conflict.body, sizeof(conflict.body), &conflict.size));
    CHECK(owner_exchange(&f.devices[0], &conflict, 60001, 24) == CORE_COAP_BAD_REQUEST);
    CHECK(owner_exchange(&f.devices[0], &reports[1], 60001,
        21 + CORE_COAP_EXCHANGE_LIFETIME_US) == CORE_COAP_BAD_REQUEST);
}

static void cross_slot_replay_never_matches_an_old_effect(void)
{
    device d = {0};
    device_init(&d, 1);
    const fixture_request request = {.id = {100}, .scope = {.kind = CORE_REQUEST_REMOVE_EDGE,
        .channel_count = 1, .allowance_us = 100,
        .bindings = {{.channel = fixture_channel(10, 20, 1, 2, 1, 2), .creating_op = {90}}}}};
    const core_admission_record *r;
    CHECK(fixture_submit(&d.admission, &request, &r) == CORE_ADMIT_ACCEPTED);
    core_edge_effect old;
    CHECK(core_edge_next(&d.edges, 2, 11, &old));
    const core_owner_report closed = {.operation = {100}, .owner = 2, .object = 1, .facts = 56};
    uint8_t body[CORE_REPORT_BYTES];
    size_t size;
    CHECK(core_owner_report_encode(&closed, body, sizeof(body), &size));
    CHECK(core_edge_owner(&d.edges, 2, body, size, 12));
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED);
    core_admission_acknowledge(&d.admission, r->id, 1, r->evidence.revision);
    core_admission_maintain(&d.admission);
    fixture_request blocker = request;
    blocker.id.value = 101;
    CHECK(fixture_submit(&d.admission, &blocker, &r) == CORE_ADMIT_ACCEPTED);
    CHECK(fixture_submit(&d.admission, &request, &r) == CORE_ADMIT_ACCEPTED);
    CHECK(r->incarnation == old.incarnation);
    CHECK(r - d.admission.records != old.record_slot);
    CHECK(!core_edge_effect_current(&d.edges, &old, 13));
    const core_admission before = d.admission;
    core_edge_rejected(&d.edges, &old, 13);
    core_edge_delivered(&d.edges, &old);
    CHECK(memcmp(&before, &d.admission, sizeof(before)) == 0);
}

static size_t channel_command(uint8_t *body, uint8_t index, const core_channel *channel)
{
    core_cbor_writer w = {.data = body, .size = CORE_EDGE_MESSAGE_BYTES};
    core_cbor_write_array(&w, 5);
    core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
    core_cbor_write_uint(&w, CORE_CHANNEL_COMMAND_KIND);
    core_cbor_write_uint(&w, 100);
    core_cbor_write_uint(&w, index);
    fixture_write_channel(&w, channel);
    CHECK(!w.failed);
    return w.offset;
}

static void sparse_owner_later_original_work_remains_valid(void)
{
    device d = {0};
    device_init(&d, 2);
    established(&d, 2, 20);
    core_channel channel = fixture_channel(10, 20, 100, 2, 1, 2);
    uint8_t body[CORE_EDGE_MESSAGE_BYTES];
    size_t size = channel_command(body, 0, &channel);
    const core_admission_record *r;
    CHECK(core_endpoint_channel_submit(&d.admission, 1, body, size, 11, true, &r) == CORE_ADMIT_ACCEPTED);
    core_edge_effect final;
    CHECK(core_edge_next(&d.edges, 1, 12, &final));
    CHECK(r->channel_known == 1 && (r->local_facts[2] & CORE_OBJECT_CLEANED));
    core_admission_maintain(&d.admission);
    CHECK(core_admission_find(&d.admission, (core_op_id){100}) == r);
    /* The transport accepted this exact final report at Kc. No unseen channel
     * descriptor is an admitted local obligation or requires aggregate state. */
    core_edge_delivered(&d.edges, &final);
    core_admission_maintain(&d.admission);
    CHECK(core_admission_find(&d.admission, (core_op_id){100}) == r);
    retire_owner_under_pressure(&d, (core_op_id){100});
    channel.service++;
    size = channel_command(body, 1, &channel);
    CHECK(core_endpoint_channel_submit(&d.admission, 1, body, size, 13, false, &r) == CORE_ADMIT_ACCEPTED);
    CHECK(r->channel_known == 2 && r->deadline_us == 0);
    CHECK(r->scope.bindings[1].channel.service == channel.service);
    CHECK(r->owner_delivered == 0);
    core_edge_delivered(&d.edges, &final);
    CHECK(r->owner_delivered == 0);
    core_admission_maintain(&d.admission);
    CHECK(core_admission_find(&d.admission, (core_op_id){100}) == r);
}

int main(void)
{
    optional_progress_after_storage_ack();
    unresolved_optional_survives_ack_and_pressure();
    retired_observer_preserves_closure_and_history();
    owner_delivery_and_same_id_slot_reuse();
    lost_reply_survives_retirement_and_reordered_duplicates();
    cross_slot_replay_never_matches_an_old_effect();
    sparse_owner_later_original_work_remains_valid();
    puts("P7 lifecycle: optional ownership, closure preservation and owner delivery passed");
    return 0;
}

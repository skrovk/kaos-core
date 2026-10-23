/* Reuse the unchanged deterministic runtime and transport fixture. */
#define main p5_fixture_main
#include "p5_test.c"
#undef main

static fixture_request removal(const fixture *f, uint64_t id, uint64_t allowance)
{
    fixture_request request = {.id = {id}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE, .logical_node_id = 10,
        .instance = {100}, .allowance_us = allowance,
        .channel_count = f->record->scope.channel_count}};
    for (size_t c = 0; c < request.scope.channel_count; ++c)
        request.scope.bindings[c] = f->record->scope.bindings[c];
    return request;
}

static const core_admission_record *remove_node(fixture *f, uint64_t id, uint64_t allowance)
{
    const fixture_request request = removal(f, id, allowance);
    const core_admission_record *r;
    CHECK(fixture_submit(&f->devices[0].admission, &request, &r) == CORE_ADMIT_ACCEPTED);
    return r;
}

static void closed(const core_admission_record *r)
{
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_report_obligations(&r->evidence) == 0);
    for (size_t o = 0; o < r->scope.object_count; ++o)
        CHECK((r->evidence.objects[o] & 56) == 56);
}

static void peer_alive(const device *d, uint64_t id)
{
    const core_admission_record *r = core_admission_find(&d->admission, (core_op_id){id});
    CHECK(r != NULL && r->node != NULL && !r->node->lifetime.closing);
    CHECK((r->evidence.objects[0] & (CORE_OBJECT_FAILED | CORE_OBJECT_CLEANUP)) == 0);
}

static void settle(fixture *f, uint64_t now)
{
    for (unsigned round = 0; round < 128; ++round) {
        bool changed = false;
        for (unsigned d = 0; d < f->device_count; ++d) step(&f->devices[d], now);
        for (unsigned from = 0; from < f->device_count; ++from) {
            for (unsigned to = 0; to < f->device_count; ++to) {
                if (from == to) continue;
                core_edge_effect effect;
                if (!core_edge_next(&f->devices[from].edges, to + 1, now, &effect)) continue;
                const message m = {.effect = effect, .from = (uint8_t)from,
                    .to = (uint8_t)to, .occupied = true};
                deliver(f, &m, now);
                changed = true;
            }
        }
        if (!changed) return;
    }
    CHECK(false);
}

static void mixed_removal(bool local)
{
    fixture f;
    const core_predicate_token yes[] = {{CORE_PREDICATE_TRUE, 0}};
    const fixture_request request = request_for(local ? 2 : 3, 2, yes, 1, local, !local);
    init(&f, &request, local, !local);
    runtime(&f)->prepared = true;
    pump(&f, 12, all);
    marker(&f, 13);
    success(&f);

    /* An unrelated binding between the two surviving peer nodes stays live. */
    const core_admission_record *unrelated = NULL;
    if (!local) {
        fixture_request edge = {.id = {101}, .scope = {
            .kind = CORE_REQUEST_ADD_EDGE, .cleanup_allowance_us = 50}};
        edge.scope.bindings[0].channel = fixture_channel(20, 30, 2, 3, 2, 3);
        CHECK(fixture_submit(&f.devices[1].admission, &edge, &unrelated) == CORE_ADMIT_ACCEPTED);
        settle(&f, 14);
        CHECK(core_report_outcome(&unrelated->evidence) == CORE_OUTCOME_SUCCEEDED);
    }
    const core_admission_record *r = remove_node(&f, 200, 100);
    pump(&f, 20, all);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_PENDING);
    CHECK(runtime(&f)->cancel_requested && runtime(&f)->dispose_requested);
    dispose(&f, 21);
    closed(r);
    CHECK(f.record->node == NULL);
    CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_SUCCEEDED);
    peer_alive(&f.devices[local ? 0 : 1], 2);
    if (!local) {
        peer_alive(&f.devices[2], 3);
        CHECK((unrelated->evidence.objects[0] & 56) == 0);
        CHECK((unrelated->evidence.objects[1] & 56) == 0);
        const core_channel_id channel = {20, 30, 7};
        CHECK(core_channel_put(&f.devices[1].endpoints, (core_instance_id){2}, channel,
                               "ok", 2) == CORE_DATA_OK);
        core_data_frame frame;
        uint64_t owner;
        CHECK(core_endpoint_next(&f.devices[1].endpoints, &owner, &frame) && owner == 3);
        CHECK(core_endpoint_receive(&f.devices[2].endpoints, 2, &frame) == CORE_DATA_OK);
        uint8_t data[16];
        size_t size;
        CHECK(core_channel_get(&f.devices[2].endpoints, (core_instance_id){3}, channel,
                               data, sizeof(data), &size) == CORE_DATA_OK);
        CHECK(size == 2 && memcmp(data, "ok", 2) == 0);
    }
}

static void pending_creator(unsigned phase)
{
    fixture f;
    const core_predicate_token required[] = {{CORE_PREDICATE_CHANNEL, 0}};
    const fixture_request request = request_for(1, 0, required, 1, false, false);
    init(&f, &request, false, false);
    if (phase != 0) runtime(&f)->prepared = true;
    if (phase == 1) pump(&f, 12, (delivery){1, 0, 0, true, true});
    if (phase == 2) {
        pump(&f, 12, all);
        CHECK(runtime(&f)->start_requested);
        runtime(&f)->main_active = true;
        runtime(&f)->init_pending = true;
    }
    const core_admission_record *r = remove_node(&f, 200, 100);
    pump(&f, 20, all);
    CHECK(!(f.record->evidence.history & CORE_HISTORY_STARTED));
    CHECK(!runtime(&f)->init_pending);
    dispose(&f, 21);
    closed(r);
    CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(!!(f.record->evidence.history & CORE_HISTORY_COMMIT) == (phase != 0));
    CHECK(!!(f.record->evidence.history & CORE_HISTORY_DISPATCH) == (phase == 2));
    peer_alive(&f.devices[1], 2);
}

static void removal_before_optional_prepare(void)
{
    fixture f;
    const core_predicate_token yes[] = {{CORE_PREDICATE_TRUE, 0}};
    const fixture_request request = request_for(1, 1, yes, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, (delivery){0, 0, 0, false, true});
    marker(&f, 13);
    success(&f);
    core_edge_effect delayed = {0};
    for (size_t i = 0; i < 64; ++i) {
        if (f.pending[i].occupied && f.pending[i].effect.kind == CORE_EDGE_PREPARE) {
            delayed = f.pending[i].effect;
            f.pending[i].occupied = false;
        }
    }
    CHECK(delayed.operation.value == 100);
    const core_admission_record *r = remove_node(&f, 200, 100);
    pump(&f, 20, all);
    dispose(&f, 21);
    closed(r);
    const core_admission_record *ignored;
    CHECK(core_endpoint_channel_submit(&f.devices[1].admission, 1, delayed.body,
        delayed.size, 22, false, &ignored) == CORE_ADMIT_DUPLICATE);
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e)
        CHECK(!f.devices[1].admission.endpoints[e].occupied);
    CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_SUCCEEDED);
    peer_alive(&f.devices[1], 2);
}

static void exited_then_removed(void)
{
    fixture f;
    const core_predicate_token required[] = {{CORE_PREDICATE_CHANNEL, 0}};
    const fixture_request request = request_for(1, 0, required, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, all);
    marker(&f, 13);
    runtime(&f)->main_active = false;
    runtime(&f)->exited = true;
    pump(&f, 20, all);
    dispose(&f, 21);
    CHECK(f.record->node == NULL);
    CHECK((f.record->evidence.objects[2] & CORE_OBJECT_CLEANED) == 0);
    fixture_request incomplete = removal(&f, 200, 100);
    incomplete.scope.channel_count = 0;
    const core_admission_record *r;
    CHECK(fixture_submit(&f.devices[0].admission, &incomplete, &r) == CORE_ADMIT_CONFLICT);
    r = remove_node(&f, 200, 100);
    pump(&f, 22, all);
    closed(r);
    CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_SUCCEEDED);
    peer_alive(&f.devices[1], 2);
}

static void separate_observers(void)
{
    fixture f;
    const core_predicate_token required[] = {{CORE_PREDICATE_CHANNEL, 0}};
    const fixture_request request = request_for(1, 0, required, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, all);
    marker(&f, 13);
    const core_admission_record *old = remove_node(&f, 200, 30);
    step(&f.devices[0], 20);
    core_edge_effect effect;
    CHECK(core_edge_next(&f.devices[0].edges, 2, 20, &effect));
    CHECK(effect.operation.value == 200 && effect.deadline_us == 40);
    runtime(&f)->main_active = false;
    runtime(&f)->disposed = true;
    pump(&f, 40, all);
    CHECK(core_report_outcome(&old->evidence) == CORE_OUTCOME_UNRESOLVED);
    const core_admission_record *fresh = remove_node(&f, 201, 100);
    pump(&f, 50, all);
    closed(fresh);
    CHECK(old->deadline_us == 40 && old->cleanup_deadline_us == 40);
    CHECK(core_report_outcome(&old->evidence) == CORE_OUTCOME_UNRESOLVED);
    CHECK((old->evidence.objects[2] & CORE_OBJECT_CLEANED) == 0);
    /* An already-sent command/reply may arrive after its transport cutoff. */
    const message late = {.effect = effect, .from = 0, .to = 1, .occupied = true};
    deliver(&f, &late, 60);
    closed(old);
    CHECK(old->evidence.history & CORE_HISTORY_CUTOFF);
    CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_SUCCEEDED);
}

static void pending_edge_on_other_coordinator(void)
{
    fixture f;
    const core_predicate_token yes[] = {{CORE_PREDICATE_TRUE, 0}};
    const fixture_request request = request_for(0, 0, yes, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, all);
    marker(&f, 13);
    fixture_request edge = {.id = {101}, .scope = {
        .kind = CORE_REQUEST_ADD_EDGE, .cleanup_allowance_us = 50}};
    edge.scope.bindings[0] = (core_binding){fixture_channel(20, 10, 2, 100, 2, 1), {101}};
    const core_admission_record *creator;
    CHECK(fixture_submit(&f.devices[1].admission, &edge, &creator) == CORE_ADMIT_ACCEPTED);
    fixture_request remove = removal(&f, 200, 100);
    remove.scope.channel_count = 1;
    remove.scope.bindings[0] = edge.scope.bindings[0];
    const core_admission_record *observer;
    CHECK(fixture_submit(&f.devices[0].admission, &remove, &observer) == CORE_ADMIT_ACCEPTED);
    settle(&f, 20);
    runtime(&f)->main_active = false;
    runtime(&f)->disposed = true;
    settle(&f, 21);
    closed(observer);
    CHECK(core_report_outcome(&creator->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_SUCCEEDED);
    peer_alive(&f.devices[1], 2);
}

static void optional_cutoff_survives_removal(void)
{
    fixture f;
    const core_predicate_token yes[] = {{CORE_PREDICATE_TRUE, 0}};
    const fixture_request request = request_for(1, 0, yes, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 12, (delivery){0, 0, 0, false, false});
    marker(&f, 13);
    step(&f.devices[0], 210); /* Optional setup deadline; cleanup ends at 260. */
    const size_t creator = (size_t)(f.record - f.devices[0].admission.records);
    CHECK(f.devices[0].edges.executions[creator].channel_cleanup_deadline_us[0] == 260);
    const core_admission_record *r = remove_node(&f, 200, 300);
    pump(&f, 220, all);
    dispose(&f, 221);
    closed(r);
    CHECK(r->cleanup_deadline_us == 310);
    CHECK(f.devices[0].edges.executions[creator].channel_cleanup_deadline_us[0] == 260);
    CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_SUCCEEDED);
}

int main(void)
{
    mixed_removal(false);
    mixed_removal(true);
    for (unsigned phase = 0; phase < 3; ++phase) pending_creator(phase);
    removal_before_optional_prepare();
    exited_then_removed();
    separate_observers();
    pending_edge_on_other_coordinator();
    optional_cutoff_survives_removal();
    puts("P6 full removal and overlapping observers passed");
    return 0;
}

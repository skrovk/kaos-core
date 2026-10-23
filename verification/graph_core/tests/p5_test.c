#include "admission_fixture.h"
#include "kaos_control.h"
#include "kaos_edge.h"
#include "kaos_node.h"

/* Runtime completion and transport acceptance are deliberately independent.
 * A held message has already been issued, but supplies no coordinator fact. */
typedef struct {
    core_runtime_state state[CORE_MAX_NODE_RESERVATIONS];
    bool locked[CORE_MAX_NODE_RESERVATIONS];
    unsigned prepares;
} fake_runtime;

typedef struct {
    core_admission admission;
    core_endpoint_service endpoints;
    core_edge_service edges;
    core_node_service nodes;
    core_control control;
    fake_runtime runtime;
} device;

typedef struct {
    core_edge_effect effect;
    uint8_t from, to;
    bool occupied;
} message;

typedef struct {
    device devices[3];
    message pending[64];
    const core_admission_record *record;
    uint8_t artifact[16];
    size_t slot;
    unsigned device_count;
} fixture;

typedef struct {
    core_channel_set ready, active, activate;
    bool prepare, cleanup;
} delivery;

static const delivery all = {UINT16_MAX, UINT16_MAX, UINT16_MAX, true, true};

static bool runtime_prepare(void *context, size_t slot, uint8_t *artifact, size_t size,
                            const uint8_t *configuration, size_t configuration_size,
                            size_t arena)
{
    fake_runtime *runtime = context;
    (void)configuration;
    CHECK(artifact != NULL && size == 16 && configuration_size == 0 && arena == 112);
    CHECK(slot < CORE_MAX_NODE_RESERVATIONS);
    ++runtime->prepares;
    return true;
}

static core_runtime_state *runtime_lock(void *context, size_t slot)
{
    fake_runtime *runtime = context;
    CHECK(!runtime->locked[slot]);
    runtime->locked[slot] = true;
    return &runtime->state[slot];
}

static void runtime_unlock(void *context, size_t slot)
{
    fake_runtime *runtime = context;
    CHECK(runtime->locked[slot]);
    runtime->locked[slot] = false;
}

static void device_init(device *d, uint64_t owner)
{
    static const uint64_t peers[] = {1, 2, 3};
    CHECK(core_admission_init(&d->admission, 4096,
        CORE_MAX_NODE_RESERVATIONS * 4096 + CORE_MAX_ENDPOINT_RESERVATIONS * CORE_ENDPOINT_BYTES));
    CHECK(core_admission_configure(&d->admission, owner, 99, peers, 3));
    core_endpoint_init(&d->endpoints, &d->admission, NULL, NULL);
    d->edges.admission = &d->admission;
    d->edges.endpoints = &d->endpoints;
    d->nodes.admission = &d->admission;
    d->nodes.runtime = (core_node_runtime){.context = &d->runtime,
        .prepare = runtime_prepare, .lock = runtime_lock, .unlock = runtime_unlock};
    d->control.admission = &d->admission;
    d->control.edges = &d->edges;
}

static void established(device *d, uint64_t id, uint64_t logical)
{
    const fixture_request request = fixture_node(id, logical);
    const core_admission_record *r;
    CHECK(fixture_submit(&d->admission, &request, &r) == CORE_ADMIT_ACCEPTED);
    core_report *report = &d->admission.records[r - d->admission.records].evidence;
    CHECK(core_report_observe(report, 0, CORE_OBJECT_READY) == CORE_REPORT_CHANGED);
    CHECK(core_report_record(report, CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH |
                             CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS) == CORE_REPORT_CHANGED);
    CHECK(core_admission_establish_node(&d->admission, (core_instance_id){id}));
}

static fixture_request request_for(unsigned channels, unsigned incoming,
                                   const core_predicate_token *tokens, size_t token_count,
                                   bool local, bool mixed)
{
    fixture_request request = fixture_node(100, 10);
    request.scope.kind = CORE_REQUEST_ADD_NODE;
    request.scope.channel_count = (uint8_t)channels;
    for (unsigned c = 0; c < channels; ++c) {
        const uint64_t peer = mixed && c == 2 ? 3 : 2;
        const uint64_t owner = local ? 1 : peer;
        core_channel channel = incoming & (1u << c) ?
            fixture_channel(10 * peer, 10, peer, 100, owner, 1) :
            fixture_channel(10, 10 * peer, 100, peer, 1, owner);
        channel.service = 7 + c;
        channel.addition_allowance_us = 200;
        request.scope.bindings[c] = (core_binding){channel, {100}};
    }
    CHECK(core_predicate_init(&request.scope.predicate, tokens, token_count, channels));
    return request;
}

static void init(fixture *f, const fixture_request *request, bool local, bool mixed)
{
    memset(f, 0, sizeof(*f));
    f->device_count = local ? 1 : mixed ? 3 : 2;
    for (unsigned d = 0; d < f->device_count; ++d) device_init(&f->devices[d], d + 1);
    established(&f->devices[local ? 0 : 1], 2, 20);
    if (mixed) established(&f->devices[2], 3, 30);
    CHECK(fixture_submit(&f->devices[0].admission, request, &f->record) == CORE_ADMIT_ACCEPTED);
    f->slot = (size_t)(f->record->node - f->devices[0].admission.nodes);
    CHECK(core_lifetime_charge(&f->record->node->lifetime, sizeof(f->artifact)));
    CHECK(core_node_artifact(&f->devices[0].nodes, f->record->id, f->artifact,
                             sizeof(f->artifact), 11));
}

static core_runtime_state *runtime(fixture *f)
{
    return &f->devices[0].runtime.state[f->slot];
}

static void step(device *d, uint64_t now)
{
    core_edge_step(&d->edges, now);
    core_node_step(&d->nodes, now);
    core_edge_step(&d->edges, now);
    core_node_step(&d->nodes, now);
}

static void deliver(fixture *f, const message *m, uint64_t now)
{
    static uint16_t message_id;
    device *from = &f->devices[m->from], *to = &f->devices[m->to];
    const core_edge_effect *effect = &m->effect;
    const char *path = core_edge_path(effect->kind);
    const uint8_t format = 60;
    core_coap_message request = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = ++message_id, .option_count = 2,
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)path, strlen(path)},
                    {CORE_COAP_CONTENT_FORMAT, &format, 1}},
        .payload = effect->body, .payload_size = effect->size};
    uint8_t packet[CORE_COAP_MAX_DATAGRAM], response[CORE_COAP_MAX_DATAGRAM];
    size_t size, response_size;
    CHECK(core_coap_encode(&request, packet, sizeof(packet), &size));
    CHECK(core_control_receive(&to->control, m->from + 1, packet, size, now,
                                 response, sizeof(response), &response_size));
    core_coap_message reply;
    CHECK(core_coap_decode(response, response_size, &reply));
    CHECK(reply.code == CORE_COAP_CREATED || reply.code == CORE_COAP_CHANGED);
    core_owner_report report;
    if (core_owner_report_decode(reply.payload, reply.payload_size, &report))
        CHECK(core_edge_owner(&from->edges, m->to + 1, reply.payload, reply.payload_size, now));
}

static bool permitted(const message *m, delivery rule)
{
    if (m->effect.kind == CORE_EDGE_PREPARE) return rule.prepare;
    if (m->effect.kind == CORE_EDGE_CANCEL) return rule.cleanup;
    CHECK(m->effect.object > 0);
    const core_channel_set bit = (core_channel_set)(1u << ((m->effect.object - 1u) / 2u));
    if (m->effect.kind == CORE_EDGE_ACTIVATE) return (rule.activate & bit) != 0;
    core_owner_report report;
    CHECK(core_owner_report_decode(m->effect.body, m->effect.size, &report));
    if (report.facts & (CORE_OBJECT_FAILED | CORE_OBJECT_CLEANUP)) return rule.cleanup;
    return ((report.facts & CORE_OBJECT_ACTIVE ? rule.active : rule.ready) & bit) != 0;
}

static void pump(fixture *f, uint64_t now, delivery rule)
{
    for (unsigned round = 0; round < 128; ++round) {
        bool changed = false;
        for (unsigned d = 0; d < f->device_count; ++d) step(&f->devices[d], now);
        for (unsigned from = 0; from < f->device_count; ++from) {
            for (unsigned to = 0; to < f->device_count; ++to) {
                if (from == to) continue;
                core_edge_effect effect;
                if (!core_edge_next(&f->devices[from].edges, to + 1, now, &effect)) continue;
                size_t index = 0;
                while (index < 64 && f->pending[index].occupied) ++index;
                CHECK(index < 64);
                f->pending[index] = (message){effect, (uint8_t)from, (uint8_t)to, true};
                changed = true;
            }
        }
        for (size_t i = 0; i < 64; ++i) {
            message *m = &f->pending[i];
            if (!m->occupied) continue;
            if (!core_edge_effect_current(&f->devices[m->from].edges, &m->effect, now)) {
                m->occupied = false;
                changed = true;
            } else if (permitted(m, rule)) {
                deliver(f, m, now);
                m->occupied = false;
                changed = true;
            }
        }
        if (!changed) return;
    }
    CHECK(false); /* no unbounded production of duplicate work */
}

static void marker(fixture *f, uint64_t now)
{
    CHECK(runtime(f)->start_requested);
    runtime(f)->main_active = true;
    runtime(f)->init_pending = true;
    step(&f->devices[0], now);
}

static void success(const fixture *f)
{
    CHECK(core_report_outcome(&f->record->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(f->record->node != NULL && f->record->node->established);
    CHECK(f->devices[0].runtime.prepares == 1);
}

static void dispose(fixture *f, uint64_t now)
{
    CHECK(runtime(f)->cancel_requested && runtime(f)->dispose_requested);
    runtime(f)->main_active = false;
    runtime(f)->configuration_active = false;
    runtime(f)->disposed = true;
    pump(f, now, all);
}

static void close_channel(fixture *f, unsigned channel)
{
    core_endpoint_close(&f->devices[0].endpoints, &f->record->scope.bindings[channel]);
}

static void complete_channel_sets(void)
{
    core_admission_record r = {.occupied = true, .channel_known = 3, .scope = {
        .kind = CORE_REQUEST_ADD_NODE, .channel_count = 2, .object_count = 5}};
    CHECK(core_report_init(&r.evidence, CORE_REQUEST_ADD_NODE, (core_op_id){100}, 1, 5));
    r.evidence.objects[1] = CORE_OBJECT_READY | CORE_OBJECT_ACTIVE;
    r.evidence.objects[4] = CORE_OBJECT_READY | CORE_OBJECT_ACTIVE;
    CHECK(core_edge_channels(&r, CORE_OBJECT_READY) == 0);
    CHECK(core_edge_channels(&r, CORE_OBJECT_ACTIVE) == 0);
    CHECK(core_edge_channels(&r, 0) == 3);
    r.evidence.objects[2] = CORE_OBJECT_READY | CORE_OBJECT_ACTIVE;
    CHECK(core_edge_channels(&r, CORE_OBJECT_READY) == 1);
    CHECK(core_edge_channels(&r, CORE_OBJECT_ACTIVE) == 1);
    r.evidence.objects[1] |= CORE_OBJECT_FAILED;
    CHECK(core_edge_channels(&r, CORE_OBJECT_ACTIVE) == 0);
    CHECK(core_edge_channels(&r, 0) == 2);
    r.channel_cleanup = 2;
    CHECK(core_edge_channels(&r, 0) == 0);
}

static void required_local_and_remote(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0}};
    for (unsigned local = 0; local < 2; ++local) {
        for (unsigned incoming = 0; incoming < 2; ++incoming) {
            fixture f;
            const fixture_request request = request_for(1, incoming, tokens, 1, local, false);
            init(&f, &request, local, false);
            pump(&f, 12, all);
            CHECK(!(f.record->evidence.history & CORE_HISTORY_COMMIT));
            CHECK(!runtime(&f)->start_requested);
            runtime(&f)->prepared = true;
            pump(&f, 20, all);
            CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == 1);
            CHECK(runtime(&f)->start_requested && !runtime(&f)->main_active);
            CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_PENDING);
            if (incoming) {
                device *source = &f.devices[local ? 0 : 1];
                const core_channel_id id = {20, 10, 7};
                CHECK(core_channel_put(&source->endpoints, (core_instance_id){2}, id, "x", 1) == CORE_DATA_OK);
                core_data_frame frame;
                uint64_t owner;
                CHECK(core_endpoint_next(&source->endpoints, &owner, &frame) && owner == 1);
                CHECK(core_endpoint_receive(&f.devices[0].endpoints, local ? 1 : 2, &frame) == CORE_DATA_OK);
                CHECK(!runtime(&f)->main_active); /* HP08: buffered before guest entry */
            }
            marker(&f, 21);
            success(&f);
            const uint8_t revision = f.record->evidence.revision;
            marker(&f, 22);
            CHECK(f.record->evidence.revision == revision);
        }
    }
}

static void alternatives_and_optional_completion(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0},
        {CORE_PREDICATE_CHANNEL, 1}, {CORE_PREDICATE_OR, 0}};
    for (unsigned sufficient = 0; sufficient < 2; ++sufficient) {
        fixture f;
        const fixture_request request = request_for(2, 0, tokens, 3, false, false);
        init(&f, &request, false, false);
        const core_request_scope original = f.record->scope;
        runtime(&f)->prepared = true;
        const core_channel_set bit = (core_channel_set)(1u << sufficient);
        pump(&f, 20, (delivery){bit, bit, bit, true, true});
        CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == bit);
        marker(&f, 21);
        success(&f);
        CHECK(core_edge_next_deadline(&f.devices[0].edges) == 210);
        pump(&f, 120, all); /* optional work outlives the node deadline */
        CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == 3);
        success(&f);
        CHECK(core_edge_next_deadline(&f.devices[0].edges) == UINT64_MAX);
        CHECK(memcmp(&original, &f.record->scope, sizeof(original)) == 0);
    }
}

static void mixed_boolean_support(void)
{
    const core_predicate_token mixed[] = {{CORE_PREDICATE_CHANNEL, 0},
        {CORE_PREDICATE_CHANNEL, 1}, {CORE_PREDICATE_OR, 0},
        {CORE_PREDICATE_CHANNEL, 2}, {CORE_PREDICATE_AND, 0}};
    const core_predicate_token pairs[] = {{CORE_PREDICATE_CHANNEL, 0},
        {CORE_PREDICATE_CHANNEL, 1}, {CORE_PREDICATE_AND, 0},
        {CORE_PREDICATE_CHANNEL, 0}, {CORE_PREDICATE_CHANNEL, 2},
        {CORE_PREDICATE_AND, 0}, {CORE_PREDICATE_OR, 0},
        {CORE_PREDICATE_CHANNEL, 1}, {CORE_PREDICATE_CHANNEL, 2},
        {CORE_PREDICATE_AND, 0}, {CORE_PREDICATE_OR, 0}};
    for (unsigned pair = 0; pair < 2; ++pair) {
        for (unsigned support = 3; support < 7; ++support) {
            if (support == 4) continue;
            fixture f;
            const fixture_request request = request_for(3, 3, pair ? pairs : mixed,
                                                        pair ? 11 : 5, false, true);
            init(&f, &request, false, true);
            runtime(&f)->prepared = true;
            pump(&f, 20, (delivery){support, support, support, true, true});
            const bool sufficient = pair || support != 3;
            CHECK(runtime(&f)->start_requested == sufficient);
            if (sufficient) {
                marker(&f, 21);
                success(&f);
            } else {
                CHECK(!(f.record->evidence.history & CORE_HISTORY_COMMIT));
            }
        }
    }
}

static void true_with_silent_optional_and_deadline(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_TRUE, 0}};
    fixture f;
    fixture_request request = request_for(2, 0, tokens, 1, false, false);
    request.scope.bindings[1].channel.addition_allowance_us = 300;
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    const delivery silent = {0, 0, 0, false, false};
    pump(&f, 20, silent);
    CHECK(core_edge_channels(f.record, CORE_OBJECT_READY) == 0);
    marker(&f, 21);
    success(&f);
    pump(&f, 210, silent);
    CHECK(f.record->channel_cleanup == 1);
    CHECK(f.record->evidence.objects[1] & CORE_OBJECT_EXPIRED);
    CHECK(!f.record->cleanup_selected && !runtime(&f)->cancel_requested);
    CHECK(core_edge_next_deadline(&f.devices[0].edges) == 260);
    pump(&f, 260, silent);
    CHECK(core_report_obligations(&f.record->evidence) != 0);
    CHECK(core_edge_next_deadline(&f.devices[0].edges) == 310);
    success(&f);
    pump(&f, 310, silent);
    CHECK(f.record->channel_cleanup == 3);
    success(&f);
}

static void selective_cancel_before_prepare_preserves_sibling(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_TRUE, 0}};
    fixture f;
    fixture_request request = request_for(2, 0, tokens, 1, false, false);
    request.scope.bindings[1].channel.addition_allowance_us = 300;
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 20, (delivery){0, 0, 0, false, true});
    message delayed = {0};
    for (size_t i = 0; i < 64; ++i)
        if (f.pending[i].occupied && f.pending[i].from == 0 &&
            f.pending[i].effect.kind == CORE_EDGE_PREPARE &&
            f.pending[i].effect.object == 2) delayed = f.pending[i];
    CHECK(delayed.occupied);
    marker(&f, 21);
    pump(&f, 210, (delivery){0, 0, 0, false, true});
    success(&f);
    CHECK(f.record->channel_cleanup == 1);
    CHECK(f.record->evidence.objects[2] & CORE_OBJECT_CLEANED);
    deliver(&f, &delayed, 211); /* late PREPARE cannot recreate cancelled A */
    pump(&f, 212, all); /* the same peer may still admit and activate B */
    CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == 2);
    CHECK(f.record->channel_cleanup == 1);
    CHECK(core_report_obligations(&f.record->evidence) == 0);
    success(&f);
}

static void maximum_channel_index_and_owner_guards(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL,
                                           CORE_MAX_REQUEST_CHANNELS - 1}};
    fixture f;
    const fixture_request request = request_for(CORE_MAX_REQUEST_CHANNELS, 0,
                                                tokens, 1, false, false);
    init(&f, &request, false, false);
    const core_report original = f.record->evidence;
    core_owner_report report = {.operation = {100}, .owner = 2,
        .object = 2 * CORE_MAX_REQUEST_CHANNELS, .facts = CORE_OBJECT_READY};
    uint8_t body[CORE_REPORT_BYTES];
    size_t size;
    CHECK(core_owner_report_encode(&report, body, sizeof(body), &size));
    CHECK(!core_edge_owner(&f.devices[0].edges, 2, body, size, 12));
    CHECK(memcmp(&original, &f.record->evidence, sizeof(original)) == 0);
    runtime(&f)->prepared = true;
    pump(&f, 20, all);
    CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) ==
          (core_channel_set)((1u << CORE_MAX_REQUEST_CHANNELS) - 1));
    marker(&f, 21);
    success(&f);
    report.facts = CORE_OBJECT_READY | CORE_OBJECT_ACTIVE;
    CHECK(core_owner_report_encode(&report, body, sizeof(body), &size));
    const core_report completed = f.record->evidence;
    CHECK(!core_edge_owner(&f.devices[0].edges, 3, body, size, 22));
    CHECK(core_edge_owner(&f.devices[0].edges, 2, body, size, 22));
    CHECK(memcmp(&completed, &f.record->evidence, sizeof(completed)) == 0);
}

static void commit_can_change_support_and_starting_cannot_wait(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0},
        {CORE_PREDICATE_CHANNEL, 1}, {CORE_PREDICATE_OR, 0}};
    fixture f;
    const fixture_request request = request_for(2, 0, tokens, 3, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 20, (delivery){1, 0, 0, true, true});
    CHECK(f.record->evidence.history & CORE_HISTORY_COMMIT);
    CHECK(!runtime(&f)->start_requested);
    close_channel(&f, 0);
    pump(&f, 21, (delivery){0, 0, 0, true, true});
    CHECK(f.record->channel_cleanup == 1 && !f.record->cleanup_selected);
    CHECK(core_edge_channels(f.record, 0) == 2);
    pump(&f, 22, all);
    CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == 2);
    marker(&f, 23);
    success(&f);

    for (unsigned alternate_active = 0; alternate_active < 2; ++alternate_active) {
        init(&f, &request, false, false);
        runtime(&f)->prepared = true;
        const core_channel_set enabled = alternate_active ? 3 : 1;
        pump(&f, 20, (delivery){enabled, enabled, enabled, true, true});
        CHECK(runtime(&f)->start_requested);
        close_channel(&f, 0);
        runtime(&f)->main_active = true;
        runtime(&f)->init_pending = true;
        pump(&f, 21, (delivery){enabled, enabled, enabled, true, true});
        if (alternate_active) success(&f);
        else {
            CHECK(f.record->cleanup_selected && runtime(&f)->cancel_requested);
            CHECK(!(f.record->evidence.history & CORE_HISTORY_STARTED));
            CHECK(f.record->evidence.history & CORE_HISTORY_DISPATCH);
        }
    }
}

static void whole_failure_histories_and_late_closure(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0}};
    for (unsigned history = 0; history < 3; ++history) {
        fixture f;
        const fixture_request request = request_for(1, 0, tokens, 1, false, false);
        init(&f, &request, false, false);
        if (history != 0) runtime(&f)->prepared = true;
        pump(&f, 20, history == 2 ? all : (delivery){1, 0, 0, true, true});
        CHECK(((f.record->evidence.history & CORE_HISTORY_COMMIT) != 0) == (history != 0));
        CHECK(((f.record->evidence.history & CORE_HISTORY_DISPATCH) != 0) == (history == 2));
        runtime(&f)->prepare_failed = history == 0;
        if (history != 0) close_channel(&f, 0);
        pump(&f, 21, (delivery){0, 0, 0, false, false});
        CHECK(f.record->cleanup_selected && runtime(&f)->cancel_requested);
        CHECK(!(f.record->evidence.history & CORE_HISTORY_STARTED));
        message delayed = {0};
        for (size_t i = 0; i < 64; ++i)
            if (f.pending[i].occupied && f.pending[i].from == 0 &&
                f.pending[i].effect.kind == CORE_EDGE_CANCEL) delayed = f.pending[i];
        CHECK(delayed.occupied);
        pump(&f, 71, (delivery){0, 0, 0, false, false});
        CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_UNRESOLVED);
        CHECK(core_report_obligations(&f.record->evidence) != 0);
        /* Actual local disposal and remote closure remain distinct evidence. */
        dispose(&f, 72);
        CHECK(f.record->node == NULL);
        CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_UNRESOLVED);
        /* The exchange stopped retrying, but an already transmitted command
         * may still arrive. Ki must accept it without interpreting Kc time. */
        deliver(&f, &delayed, 73);
        pump(&f, 73, all);
        CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_FAILED_CLEAN);
        CHECK(core_report_obligations(&f.record->evidence) == 0);
        CHECK(f.devices[1].admission.nodes[0].established);
    }
}

static void init_expiry_and_exit_races(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0}};
    for (unsigned variant = 0; variant < 4; ++variant) {
        fixture f;
        const fixture_request request = request_for(1, 0, tokens, 1, false, false);
        init(&f, &request, false, false);
        runtime(&f)->prepared = true;
        pump(&f, 20, all);
        if (variant == 0) {
            marker(&f, 109);
            runtime(&f)->exited = true;
            pump(&f, 110, all);
            CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_SUCCEEDED);
        } else {
            runtime(&f)->main_active = true;
            runtime(&f)->init_pending = true;
            if (variant == 3) runtime(&f)->exited = true;
            pump(&f, variant == 3 ? 21 : 109 + variant, all);
            CHECK(f.record->cleanup_selected);
            CHECK(!(f.record->evidence.history & CORE_HISTORY_STARTED));
            CHECK(core_report_outcome(&f.record->evidence) != CORE_OUTCOME_SUCCEEDED);
        }
    }
}

static void cleanup_expansion_preserves_exhausted_window(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0},
        {CORE_PREDICATE_CHANNEL, 1}, {CORE_PREDICATE_OR, 0}};
    fixture f;
    const fixture_request request = request_for(2, 0, tokens, 3, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepared = true;
    pump(&f, 20, (delivery){1, 0, 0, true, false});
    CHECK(f.record->evidence.history & CORE_HISTORY_COMMIT);
    close_channel(&f, 0);
    pump(&f, 21, (delivery){0, 0, 0, false, false});
    CHECK(!f.record->cleanup_selected);
    CHECK(core_edge_next_deadline(&f.devices[0].edges) == 71);
    pump(&f, 71, (delivery){0, 0, 0, false, false});
    close_channel(&f, 1);
    pump(&f, 80, (delivery){0, 0, 0, false, false});
    CHECK(f.record->cleanup_selected && runtime(&f)->cancel_requested);
    CHECK(core_edge_next_deadline(&f.devices[0].edges) == 130);
    CHECK(core_node_next_deadline(&f.devices[0].nodes) == 130);
    for (size_t i = 0; i < 64; ++i)
        CHECK(!f.pending[i].occupied || f.pending[i].from != 0 ||
              f.pending[i].effect.kind != CORE_EDGE_CANCEL ||
              f.pending[i].effect.object != 2); /* A's retry window stayed closed */
    pump(&f, 130, (delivery){0, 0, 0, false, false});
    CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_UNRESOLVED);
    CHECK(!(f.record->evidence.history & CORE_HISTORY_DISPATCH));
}

static void entry_checks_current_local_lifetimes(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0},
        {CORE_PREDICATE_CHANNEL, 1}, {CORE_PREDICATE_OR, 0}};
    fixture f;
    const fixture_request request = request_for(2, 0, tokens, 3, false, false);
    init(&f, &request, false, false);
    CHECK(!core_node_start_allowed(&f.devices[0].nodes, f.slot, 12));
    runtime(&f)->prepared = true;
    pump(&f, 20, all);
    CHECK(core_node_start_allowed(&f.devices[0].nodes, f.slot, 21));
    CHECK(!core_node_start_allowed(&f.devices[0].nodes, f.slot, 110));
    CHECK(!core_node_start_allowed(&f.devices[0].nodes, CORE_MAX_NODE_RESERVATIONS, 21));
    close_channel(&f, 0);
    CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == 3);
    CHECK(core_node_start_allowed(&f.devices[0].nodes, f.slot, 21));
    close_channel(&f, 1);
    /* No control/evidence step has observed either closure yet. The worker
     * must use live local permissions as well as retained remote ACTIVE. */
    CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == 3);
    CHECK(!core_node_start_allowed(&f.devices[0].nodes, f.slot, 21));
}

static void every_local_pair_is_sufficient(void)
{
    const core_predicate_token pairs[] = {{CORE_PREDICATE_CHANNEL, 0},
        {CORE_PREDICATE_CHANNEL, 1}, {CORE_PREDICATE_AND, 0},
        {CORE_PREDICATE_CHANNEL, 0}, {CORE_PREDICATE_CHANNEL, 2},
        {CORE_PREDICATE_AND, 0}, {CORE_PREDICATE_OR, 0},
        {CORE_PREDICATE_CHANNEL, 1}, {CORE_PREDICATE_CHANNEL, 2},
        {CORE_PREDICATE_AND, 0}, {CORE_PREDICATE_OR, 0}};
    for (unsigned failed = 0; failed < 3; ++failed) {
        fixture f;
        const fixture_request request = request_for(3, 3, pairs, 11, true, false);
        init(&f, &request, true, false);
        close_channel(&f, failed);
        runtime(&f)->prepared = true;
        pump(&f, 20, all);
        CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == (7u & ~(1u << failed)));
        CHECK(core_node_start_allowed(&f.devices[0].nodes, f.slot, 21));
        marker(&f, 21);
        success(&f);
    }
}

static void maximum_local_channels(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL,
                                           CORE_MAX_REQUEST_CHANNELS - 1}};
    fixture f;
    const fixture_request request = request_for(CORE_MAX_REQUEST_CHANNELS,
        (1u << CORE_MAX_REQUEST_CHANNELS) - 1, tokens, 1, true, false);
    init(&f, &request, true, false);
    runtime(&f)->prepared = true;
    pump(&f, 20, all);
    CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) ==
          (core_channel_set)((1u << CORE_MAX_REQUEST_CHANNELS) - 1));
    marker(&f, 21);
    success(&f);
}

static void late_rejection_cannot_cleanup_completed_channel(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0}};
    fixture f;
    const fixture_request request = request_for(1, 0, tokens, 1, false, false);
    init(&f, &request, false, false);
    pump(&f, 12, (delivery){0, 0, 0, false, true});
    core_edge_effect original = {0};
    bool issued = false;
    for (size_t i = 0; i < 64; ++i)
        if (f.pending[i].occupied && f.pending[i].from == 0 &&
            f.pending[i].effect.kind == CORE_EDGE_PREPARE) {
            original = f.pending[i].effect;
            issued = true;
        }
    CHECK(issued);
    runtime(&f)->prepared = true;
    pump(&f, 20, all);
    marker(&f, 21);
    success(&f);
    const core_report completed = f.record->evidence;
    CHECK(!core_edge_effect_current(&f.devices[0].edges, &original, 22));
    core_edge_rejected(&f.devices[0].edges, &original, 22);
    CHECK(!f.record->cleanup_selected && f.record->channel_cleanup == 0);
    CHECK(memcmp(&completed, &f.record->evidence, sizeof(completed)) == 0);
    core_edge_effect effect;
    CHECK(!core_edge_next(&f.devices[0].edges, 2, 23, &effect));
    CHECK(core_edge_channels(f.record, CORE_OBJECT_ACTIVE) == 1);
    success(&f);
}

static void whole_cleanup_uses_first_selection_time(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0}};
    fixture f;
    const fixture_request request = request_for(1, 0, tokens, 1, false, false);
    init(&f, &request, false, false);
    runtime(&f)->prepare_failed = true;
    core_node_step(&f.devices[0].nodes, 20);
    CHECK(f.record->cleanup_selected && core_node_next_deadline(&f.devices[0].nodes) == 70);
    /* Endpoint work is first serviced later; this is the same cleanup attempt,
     * so scheduling delay must not grant it another 15 microseconds. */
    core_edge_step(&f.devices[0].edges, 35);
    CHECK(core_edge_next_deadline(&f.devices[0].edges) == 70);
    core_edge_effect cleanup;
    CHECK(core_edge_next(&f.devices[0].edges, 2, 35, &cleanup));
    CHECK(cleanup.kind == CORE_EDGE_CANCEL && cleanup.deadline_us == 70);
    core_edge_step(&f.devices[0].edges, 70);
    CHECK(core_report_outcome(&f.record->evidence) == CORE_OUTCOME_UNRESOLVED);
}

static void compact_block1_releases_workspace(void)
{
    const core_predicate_token tokens[] = {{CORE_PREDICATE_CHANNEL, 0}};
    fixture f;
    const fixture_request request = request_for(1, 0, tokens, 1, false, false);
    init(&f, &request, false, false);
    core_edge_effect effect;
    CHECK(core_edge_next(&f.devices[0].edges, 2, 20, &effect));
    const uint8_t format = 60, block = 6; /* complete first 1024-byte block */
    uint8_t size_option[4];
    const size_t size_length = core_coap_uint((uint32_t)effect.size, size_option);
    for (uint8_t tag = 1; tag <= 2; ++tag) {
        core_coap_message command = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
            .message_id = tag, .option_count = 5,
            .options = {{CORE_COAP_URI_PATH, (const uint8_t *)"prepare", 7},
                        {CORE_COAP_CONTENT_FORMAT, &format, 1},
                        {CORE_COAP_BLOCK1, &block, 1},
                        {CORE_COAP_SIZE1, size_option, size_length},
                        {CORE_COAP_REQUEST_TAG, &tag, 1}},
            .payload = effect.body, .payload_size = effect.size};
        uint8_t bytes[CORE_COAP_MAX_DATAGRAM], response[CORE_COAP_MAX_DATAGRAM];
        size_t size, response_size;
        CHECK(core_coap_encode(&command, bytes, sizeof(bytes), &size));
        CHECK(core_control_receive(&f.devices[1].control, 1, bytes, size, 20,
            response, sizeof(response), &response_size));
        core_coap_message reply;
        CHECK(core_coap_decode(response, response_size, &reply));
        CHECK(reply.code == CORE_COAP_CHANGED);
        CHECK(!f.devices[1].control.assembly.active && !f.devices[1].control.assembly.complete);
    }
}

int main(void)
{
    complete_channel_sets();
    required_local_and_remote();
    alternatives_and_optional_completion();
    mixed_boolean_support();
    true_with_silent_optional_and_deadline();
    selective_cancel_before_prepare_preserves_sibling();
    maximum_channel_index_and_owner_guards();
    commit_can_change_support_and_starting_cannot_wait();
    whole_failure_histories_and_late_closure();
    init_expiry_and_exit_races();
    cleanup_expansion_preserves_exhausted_window();
    entry_checks_current_local_lifetimes();
    every_local_pair_is_sufficient();
    maximum_local_channels();
    late_rejection_cannot_cleanup_completed_channel();
    whole_cleanup_uses_first_selection_time();
    compact_block1_releases_workspace();
    puts("P5: Boolean gates, independent channels, startup races, optional deadlines and cleanup passed");
    return 0;
}

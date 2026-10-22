#include "kaos_edge.h"
#include "kaos_admission_internal.h"
#include "kaos_cbor.h"
#include "kaos_coap.h"

static core_admission_record *find(core_edge_service *s, core_op_id id)
{
    const core_admission_record *r = core_admission_find(s->admission, id);
    return r == NULL ? NULL : &s->admission->records[r - s->admission->records];
}

bool core_edge_supported(const core_admission_record *r)
{
    return r != NULL && r->occupied &&
           (r->scope.kind == CORE_REQUEST_ADD_NODE || r->scope.kind == CORE_REQUEST_REMOVE_NODE ||
            ((r->scope.kind == CORE_REQUEST_ADD_EDGE ||
              r->scope.kind == CORE_REQUEST_REMOVE_EDGE) &&
             r->scope.channel_count == 1));
}

static bool removal(const core_admission_record *r)
{
    return r->scope.kind == CORE_REQUEST_REMOVE_NODE || r->scope.kind == CORE_REQUEST_REMOVE_EDGE;
}

static uint8_t first_object(const core_admission_record *r)
{
    return r->scope.kind == CORE_REQUEST_ADD_NODE || r->scope.kind == CORE_REQUEST_REMOVE_NODE ? 1
                                                                                               : 0;
}

static bool known(const core_admission_record *r, size_t c)
{
    return (r->channel_known & (1u << c)) != 0;
}

static bool succeeded(const core_admission_record *r)
{
    return (r->evidence.history & CORE_HISTORY_SUCCESS) != 0;
}

static uint8_t pair_facts(const core_admission_record *r, size_t c)
{
    const size_t i = first_object(r) + 2 * c;
    return r->evidence.objects[i] | r->evidence.objects[i + 1];
}

static bool complete(const core_admission_record *r, size_t c, uint8_t fact)
{
    const size_t i = first_object(r) + 2 * c;
    return (r->evidence.objects[i] & fact) && (r->evidence.objects[i + 1] & fact);
}

core_channel_set core_edge_channels(const core_admission_record *r, uint8_t fact)
{
    core_channel_set result = 0;
    if (r->cleanup_selected)
        return 0;
    for (size_t c = 0; c < r->scope.channel_count; ++c) {
        if (!known(r, c) || (r->channel_cleanup & (1u << c)) ||
            (pair_facts(r, c) & (CORE_OBJECT_FAILED | CORE_OBJECT_CLEANUP | CORE_OBJECT_EXPIRED)))
            continue;
        if (fact == 0 || complete(r, c, fact))
            result |= (core_channel_set)(1u << c);
    }
    return result;
}

static uint64_t owner(const core_admission_record *r, uint8_t object)
{
    const size_t index = object - first_object(r);
    const core_channel *c = &r->scope.bindings[index / 2].channel;
    return index % 2 == 0 ? c->source_owner : c->destination_owner;
}

static uint64_t setup_deadline(const core_admission_record *r, size_t c)
{
    /* Admission checked both additions for overflow, at this same origin. */
    return r->deadline_us - r->scope.allowance_us +
           r->scope.bindings[c].channel.addition_allowance_us;
}

static core_edge_execution *execution(core_edge_service *s, core_admission_record *r)
{
    core_edge_execution *e = &s->executions[r - s->admission->records];
    if (!e->occupied || e->id.value != r->id.value || e->incarnation != r->incarnation)
        *e = (core_edge_execution){.id = r->id, .incarnation = r->incarnation, .occupied = true};
    return e;
}

static void channel_cleanup(core_edge_service *s, core_admission_record *r, size_t c, uint64_t now)
{
    core_edge_execution *e = execution(s, r);
    const core_channel_set bit = (core_channel_set)(1u << c);
    if (!(e->channel_cleanup_started & bit)) {
        e->channel_cleanup_started |= bit;
        uint64_t *end = &e->channel_cleanup_deadline_us[c];
        if (r->endpoint_only)
            *end = UINT64_MAX;
        else if (r->cleanup_selected && r->cleanup_deadline_us != 0)
            *end = r->cleanup_deadline_us;
        else if (removal(r))
            *end = r->deadline_us;
        else if (!core_deadline(now, r->scope.cleanup_allowance_us, end))
            *end = now;
    }
    (void)core_admission_close_channel(s->admission, r->id, (uint8_t)c);
    core_endpoint_close(s->endpoints, &r->scope.bindings[c]);
    for (uint8_t role = 0; role < 2; ++role) {
        const uint8_t object = (uint8_t)(first_object(r) + 2 * c + role);
        /* Selection establishes an obligation, never owner closure. Keep
         * accepted owner snapshots separate so in-flight READY remains valid. */
        if (!r->endpoint_only)
            (void)core_report_observe(&r->evidence, object,
                                      r->evidence.objects[object] | CORE_OBJECT_CLEANUP);
        if (owner(r, object) == s->admission->local_device)
            e->cleanup_issued |= UINT32_C(1) << object;
    }
}

static void cleanup(core_edge_service *s, core_admission_record *r, uint64_t now)
{
    core_edge_execution *e = execution(s, r);
    if (r->cleanup_deadline_us == 0) {
        if (r->endpoint_only || succeeded(r))
            r->cleanup_deadline_us = UINT64_MAX;
        else if (removal(r))
            r->cleanup_deadline_us = r->deadline_us;
        else if (!core_deadline(now, r->scope.cleanup_allowance_us, &r->cleanup_deadline_us))
            r->cleanup_deadline_us = now;
    }
    if (!r->endpoint_only && !succeeded(r))
        (void)core_report_record(&r->evidence, CORE_HISTORY_CLEANUP);
    (void)core_admission_close_scope(s->admission, r->id);
    for (size_t c = 0; c < r->scope.channel_count; ++c) {
        if (!known(r, c))
            continue;
        /* A runtime exit after success grants only local reclamation of an
         * established channel. Distributed removal requires a fresh request. */
        if (succeeded(r) && complete(r, c, CORE_OBJECT_ACTIVE) &&
            !(e->channel_cleanup_started & (1u << c)))
            continue;
        channel_cleanup(s, r, c, now);
    }
}

static void cutoff(core_admission_record *r, core_edge_execution *e, uint64_t now)
{
    if (r->endpoint_only)
        return;
    for (size_t c = 0; c < r->scope.channel_count; ++c) {
        if (!(e->channel_cleanup_started & (1u << c)) ||
            core_before_deadline(now, e->channel_cleanup_deadline_us[c]))
            continue;
        e->channel_cutoff |= (core_channel_set)(1u << c);
        for (uint8_t role = 0; role < 2; ++role) {
            const uint8_t object = (uint8_t)(first_object(r) + 2 * c + role);
            if (!(r->evidence.objects[object] & CORE_OBJECT_CLEANED))
                (void)core_report_observe(&r->evidence, object,
                                          r->evidence.objects[object] | CORE_OBJECT_EXPIRED);
        }
    }
    if (!succeeded(r) && r->cleanup_selected && r->cleanup_deadline_us != 0 &&
        !core_before_deadline(now, r->cleanup_deadline_us) &&
        core_report_obligations(&r->evidence) != 0)
        (void)core_report_record(&r->evidence, CORE_HISTORY_CUTOFF);
}

static core_report_result observation_result(const core_edge_service *s,
                                             const core_admission_record *r, uint64_t sender,
                                             uint8_t object, uint8_t facts)
{
    if (!core_edge_supported(r) || r->endpoint_only || object < first_object(r) ||
        object >= r->scope.object_count || !known(r, (object - first_object(r)) / 2) ||
        owner(r, object) != sender)
        return CORE_REPORT_INVALID;

    const core_edge_execution *e = &s->executions[r - s->admission->records];
    if (!e->occupied || e->id.value != r->id.value || e->incarnation != r->incarnation)
        return CORE_REPORT_INVALID;

    const uint32_t bit = UINT32_C(1) << object;
    if (!(e->prepare_issued & bit) && !(e->cleanup_issued & bit))
        return CORE_REPORT_INVALID;
    if ((facts & CORE_OBJECT_READY) && !(e->prepare_issued & bit))
        return CORE_REPORT_INVALID;
    if ((facts & CORE_OBJECT_ACTIVE) && !(e->activate_issued & bit))
        return CORE_REPORT_INVALID;

    /* Validate the owner's cumulative stream without coordinator-added bits. */
    core_report next;
    (void)core_report_init(&next, r->scope.kind, r->id, r->scope.coordinator,
                           r->scope.object_count);
    if (e->emitted[object])
        (void)core_report_observe(&next, object, e->emitted[object]);
    return core_report_observe(&next, object, facts);
}

static bool observe(core_edge_service *s, core_admission_record *r, uint64_t sender, uint8_t object,
                    uint8_t facts, uint64_t now)
{
    const core_report_result result = observation_result(s, r, sender, object, facts);
    if (result == CORE_REPORT_INVALID || result == CORE_REPORT_CONFLICT ||
        result == CORE_REPORT_EXHAUSTED)
        return false;
    if (result != CORE_REPORT_CHANGED)
        return true;

    core_edge_execution *e = execution(s, r);
    cutoff(r, e, now);
    const size_t c = (object - first_object(r)) / 2;
    const bool established = complete(r, c, CORE_OBJECT_ACTIVE);
    e->emitted[object] = facts;
    (void)core_report_observe(&r->evidence, object, r->evidence.objects[object] | facts);

    if ((facts & (CORE_OBJECT_FAILED | CORE_OBJECT_CLEANUP)) && !(succeeded(r) && established)) {
        if (r->scope.kind == CORE_REQUEST_ADD_NODE)
            channel_cleanup(s, r, c, now);
        else
            cleanup(s, r, now);
    }
    return true;
}

static void local_observations(core_edge_service *s, core_admission_record *r, uint64_t now)
{
    for (uint8_t i = first_object(r); i < r->scope.object_count; ++i)
        if (known(r, (i - first_object(r)) / 2) && owner(r, i) == s->admission->local_device)
            (void)observe(s, r, s->admission->local_device, i, r->local_facts[i], now);
}

static void apply_edge_removals(core_edge_service *s, uint64_t now)
{
    /* Removal owns the decision before any pending creator is serviced, even
     * when its record follows that creator and the node service has not run. */
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *r = &s->admission->records[i];
        if (core_edge_supported(r) && removal(r))
            cleanup(s, r, now);
    }
}

static void advance_channel_preparation_and_cleanup(core_edge_service *s, uint64_t now)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *r = &s->admission->records[i];
        if (!core_edge_supported(r))
            continue;

        core_edge_execution *e = execution(s, r);
        if (r->cleanup_selected && !removal(r))
            cleanup(s, r, now);

        for (size_t c = 0; c < r->scope.channel_count; ++c) {
            if (!known(r, c))
                continue;

            if (r->channel_cleanup & (1u << c)) {
                if (!(succeeded(r) && complete(r, c, CORE_OBJECT_ACTIVE) &&
                      !(e->channel_cleanup_started & (1u << c))))
                    channel_cleanup(s, r, c, now);
                continue;
            }

            if (!r->endpoint_only && !complete(r, c, CORE_OBJECT_ACTIVE) &&
                !core_before_deadline(now, setup_deadline(r, c))) {
                const uint8_t object = (uint8_t)(first_object(r) + 2 * c);
                (void)core_report_observe(&r->evidence, object,
                                          r->evidence.objects[object] | CORE_OBJECT_EXPIRED);
                if (r->scope.kind == CORE_REQUEST_ADD_NODE)
                    channel_cleanup(s, r, c, now);
                else
                    cleanup(s, r, now);
                continue;
            }

            for (uint8_t role = 0; role < 2; ++role) {
                const uint8_t object = (uint8_t)(first_object(r) + 2 * c + role);
                if (owner(r, object) == s->admission->local_device)
                    e->prepare_issued |= UINT32_C(1) << object;
            }
        }

        cutoff(r, e, now);
    }
}

static void activate_local_channel_endpoints(core_edge_service *s, core_admission_record *r,
                                             core_edge_execution *e, uint64_t now)
{
    for (size_t c = 0; c < r->scope.channel_count; ++c) {
        if (!(core_edge_channels(r, CORE_OBJECT_READY) & (1u << c)))
            continue;

        /* Receiver first, in either direction, including local channels. */
        for (int role = 1; role >= 0; --role) {
            const uint8_t object = (uint8_t)(first_object(r) + 2 * c + role);
            const uint32_t bit = UINT32_C(1) << object;
            if (owner(r, object) != s->admission->local_device || (e->activate_issued & bit) ||
                (role == 0 && !(r->evidence.objects[object + 1] & CORE_OBJECT_ACTIVE)))
                continue;

            e->activate_issued |= bit;
            if (!core_endpoint_activate(s->endpoints, &r->scope.bindings[c], role == 0)) {
                if (r->scope.kind == CORE_REQUEST_ADD_NODE)
                    channel_cleanup(s, r, c, now);
                else
                    cleanup(s, r, now);
                break;
            }

            local_observations(s, r, now);
        }
    }
}

static void advance_edge_activation(core_edge_service *s, uint64_t now)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *r = &s->admission->records[i];
        if (!core_edge_supported(r) || r->endpoint_only)
            continue;

        core_edge_execution *e = execution(s, r);
        local_observations(s, r, now);
        if (r->cleanup_selected)
            continue;

        if (r->scope.kind == CORE_REQUEST_ADD_EDGE && core_edge_channels(r, CORE_OBJECT_READY) != 0)
            (void)core_report_record(&r->evidence, CORE_HISTORY_COMMIT);
        if (!(r->evidence.history & CORE_HISTORY_COMMIT))
            continue;

        activate_local_channel_endpoints(s, r, e, now);
        if (r->scope.kind == CORE_REQUEST_ADD_EDGE && core_edge_channels(r, CORE_OBJECT_ACTIVE))
            (void)core_report_record(&r->evidence, CORE_HISTORY_SUCCESS);
    }
}

void core_edge_step(core_edge_service *s, uint64_t now)
{
    apply_edge_removals(s, now);
    advance_channel_preparation_and_cleanup(s, now);
    core_endpoint_step(s->endpoints);
    advance_edge_activation(s, now);
}

uint64_t core_edge_next_deadline(const core_edge_service *s)
{
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        const core_admission_record *r = &s->admission->records[i];
        const core_edge_execution *e = &s->executions[i];
        if (!core_edge_supported(r) || r->endpoint_only)
            continue;
        for (size_t c = 0; c < r->scope.channel_count; ++c) {
            if (!known(r, c))
                continue;
            if (!(r->channel_cleanup & (1u << c)) && !complete(r, c, CORE_OBJECT_ACTIVE)) {
                const uint64_t end = setup_deadline(r, c);
                if (end < next)
                    next = end;
            }
            if (e->occupied && e->id.value == r->id.value && e->incarnation == r->incarnation &&
                (e->channel_cleanup_started & (1u << c))) {
                const uint8_t object = (uint8_t)(first_object(r) + 2 * c);
                const uint32_t missing =
                    core_report_obligations(&r->evidence) & (UINT32_C(3) << object);
                if (missing && !(e->channel_cutoff & (1u << c)) &&
                    e->channel_cleanup_deadline_us[c] < next)
                    next = e->channel_cleanup_deadline_us[c];
            }
        }
        if (r->cleanup_selected && !succeeded(r) && !(r->evidence.history & CORE_HISTORY_CUTOFF) &&
            core_report_obligations(&r->evidence) && r->cleanup_deadline_us < next)
            next = r->cleanup_deadline_us;
    }
    return next;
}

static bool encode_channel_operation_request(const core_admission_record *r, size_t c,
                                             core_edge_effect *effect)
{
    core_cbor_writer w = {.data = effect->body, .size = sizeof(effect->body)};
    if (r->scope.kind == CORE_REQUEST_ADD_NODE) {
        core_cbor_write_array(&w, 5);
        core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
        core_cbor_write_uint(&w, CORE_CHANNEL_COMMAND_KIND);
        core_cbor_write_uint(&w, r->id.value);
        core_cbor_write_uint(&w, c);
        core_channel_encode(&w, &r->scope.bindings[c].channel);
    } else if (r->scope.kind == CORE_REQUEST_REMOVE_NODE) {
        /* Keep this observer's original index and operation identity. The
         * binding's creation and target instance are independent identities. */
        core_cbor_write_array(&w, 6);
        core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
        core_cbor_write_uint(&w, CORE_REMOVAL_COMMAND_KIND);
        core_cbor_write_uint(&w, r->id.value);
        core_cbor_write_uint(&w, c);
        core_cbor_write_uint(&w, r->scope.instance.value);
        core_cbor_write_array(&w, 2);
        core_channel_encode(&w, &r->scope.bindings[c].channel);
        core_cbor_write_uint(&w, r->scope.bindings[c].creating_op.value);
    } else {
        uint8_t body[CORE_EDGE_MESSAGE_BYTES];
        core_cbor_writer b = {.data = body, .size = sizeof(body)};
        core_cbor_write_array(&b, 2);
        if (r->scope.kind == CORE_REQUEST_REMOVE_EDGE)
            core_cbor_write_array(&b, 2);
        core_channel_encode(&b, &r->scope.bindings[0].channel);
        if (r->scope.kind == CORE_REQUEST_REMOVE_EDGE)
            core_cbor_write_uint(&b, r->scope.bindings[0].creating_op.value);
        core_cbor_write_uint(&b, r->scope.kind == CORE_REQUEST_REMOVE_EDGE
                                     ? r->scope.allowance_us
                                     : r->scope.cleanup_allowance_us);
        if (b.failed)
            return false;
        core_cbor_write_array(&w, 4);
        core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
        core_cbor_write_uint(&w, r->scope.kind);
        core_cbor_write_uint(&w, r->id.value);
        core_cbor_write_bytes(&w, body, b.offset);
    }
    effect->size = w.offset;
    return !w.failed;
}

static bool encode_endpoint_owner_report(const core_admission_record *r, uint64_t local_owner,
                                         uint8_t object, core_edge_effect *effect)
{
    const core_owner_report report = {.operation = r->id,
                                      .owner = local_owner,
                                      .object = object,
                                      .facts = r->local_facts[object]};
    return core_owner_report_encode(&report, effect->body, sizeof(effect->body), &effect->size);
}

static bool encode_endpoint_activation_command(core_op_id id, uint8_t object,
                                               core_edge_effect *effect)
{
    core_cbor_writer w = {.data = effect->body, .size = sizeof(effect->body)};
    core_cbor_write_array(&w, 4);
    core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
    core_cbor_write_uint(&w, CORE_ACTIVATE_KIND);
    core_cbor_write_uint(&w, id.value);
    core_cbor_write_uint(&w, object);
    if (w.failed)
        return false;
    effect->size = w.offset;
    return true;
}

/* Construct one effect and record its issuance only after encoding succeeds.
 * Delivery and retry handling belong to the caller. */
static bool issue_next_edge_effect(core_edge_service *s, core_admission_record *r, uint64_t peer,
                                   uint64_t now, core_edge_effect *effect)
{
    core_edge_execution *e = execution(s, r);
    *effect = (core_edge_effect){.operation = r->id,
                                 .incarnation = r->incarnation,
                                 .record_slot = (uint8_t)(r - s->admission->records),
                                 .peer = peer};

    for (uint8_t i = first_object(r); i < r->scope.object_count; ++i) {
        const size_t c = (i - first_object(r)) / 2;
        if (!known(r, c))
            continue;

        effect->object = i;
        if (r->endpoint_only) {
            if (r->scope.coordinator != peer || owner(r, i) != s->admission->local_device ||
                r->local_facts[i] == e->emitted[i])
                continue;
            if (!encode_endpoint_owner_report(r, s->admission->local_device, i, effect) ||
                !core_deadline(now, CORE_COAP_EXCHANGE_LIFETIME_US, &effect->deadline_us))
                return false;
            effect->kind = CORE_EDGE_OWNER;
            e->emitted[i] = r->local_facts[i];
            return true;
        }

        const uint32_t bit = UINT32_C(1) << i;
        if (owner(r, i) != peer)
            continue;

        if (r->channel_cleanup & (1u << c)) {
            if (!(e->channel_cleanup_started & (1u << c)) || (e->cleanup_issued & bit) ||
                !core_before_deadline(now, e->channel_cleanup_deadline_us[c]))
                continue;
            effect->kind = removal(r) ? CORE_EDGE_PREPARE : CORE_EDGE_CANCEL;
            effect->deadline_us = e->channel_cleanup_deadline_us[c];
            if (!encode_channel_operation_request(r, c, effect))
                return false;
            e->cleanup_issued |= bit;
            return true;
        }

        if (complete(r, c, CORE_OBJECT_ACTIVE) || !core_before_deadline(now, setup_deadline(r, c)))
            continue;
        effect->deadline_us = setup_deadline(r, c);
        if (!(e->prepare_issued & bit)) {
            effect->kind = CORE_EDGE_PREPARE;
            if (!encode_channel_operation_request(r, c, effect))
                return false;
            e->prepare_issued |= bit;
            return true;
        }

        if ((r->evidence.history & CORE_HISTORY_COMMIT) && !(e->activate_issued & bit) &&
            (core_edge_channels(r, CORE_OBJECT_READY) & (1u << c)) &&
            ((i - first_object(r)) % 2 == 1 || (r->evidence.objects[i + 1] & CORE_OBJECT_ACTIVE))) {
            if (!encode_endpoint_activation_command(r->id, i, effect))
                return false;
            effect->kind = CORE_EDGE_ACTIVATE;
            e->activate_issued |= bit;
            return true;
        }
    }
    return false;
}

bool core_edge_next(core_edge_service *s, uint64_t peer, uint64_t now, core_edge_effect *effect)
{
    if (peer == s->admission->local_device)
        return false;

    core_edge_step(s, now);

    for (size_t n = 0; n < CORE_MAX_ADMISSION_RECORDS; ++n) {
        const size_t i = (s->next_record + n) % CORE_MAX_ADMISSION_RECORDS;
        core_admission_record *r = &s->admission->records[i];
        if (core_edge_supported(r) && issue_next_edge_effect(s, r, peer, now, effect)) {
            s->next_record = (i + 1) % CORE_MAX_ADMISSION_RECORDS;
            return true;
        }
    }
    return false;
}

const char *core_edge_path(core_edge_effect_kind kind)
{
    switch (kind) {
    case CORE_EDGE_PREPARE:
        return "prepare";
    case CORE_EDGE_ACTIVATE:
        return "activate";
    case CORE_EDGE_CANCEL:
        return "cancel";
    case CORE_EDGE_OWNER:
        return "owner";
    }
    return "";
}

bool core_edge_effect_current(const core_edge_service *s, const core_edge_effect *effect,
                              uint64_t now)
{
    if (!core_before_deadline(now, effect->deadline_us))
        return false;
    const core_admission_record *r = core_admission_find(s->admission, effect->operation);
    if (!core_edge_supported(r) || r->incarnation != effect->incarnation ||
        r - s->admission->records != effect->record_slot)
        return false;
    if (effect->kind == CORE_EDGE_OWNER || effect->kind == CORE_EDGE_CANCEL || removal(r))
        return true;
    const size_t c = (effect->object - first_object(r)) / 2;
    return !r->cleanup_selected && !(r->channel_cleanup & (1u << c)) &&
           !complete(r, c, CORE_OBJECT_ACTIVE);
}

void core_edge_rejected(core_edge_service *s, const core_edge_effect *effect, uint64_t now)
{
    core_admission_record *r = find(s, effect->operation);
    if (!core_edge_supported(r) || r->incarnation != effect->incarnation ||
        r - s->admission->records != effect->record_slot || r->endpoint_only ||
        (effect->kind != CORE_EDGE_PREPARE && effect->kind != CORE_EDGE_ACTIVATE))
        return;
    const size_t c = (effect->object - first_object(r)) / 2;
    if (succeeded(r) && complete(r, c, CORE_OBJECT_ACTIVE))
        return;
    if (r->scope.kind == CORE_REQUEST_ADD_NODE)
        channel_cleanup(s, r, c, now);
    else if (!succeeded(r))
        cleanup(s, r, now);
}

void core_edge_delivered(core_edge_service *s, const core_edge_effect *effect)
{
    core_admission_record *r = find(s, effect->operation);
    core_owner_report report;
    if (!core_edge_supported(r) || !r->endpoint_only || r->incarnation != effect->incarnation ||
        r - s->admission->records != effect->record_slot || effect->kind != CORE_EDGE_OWNER ||
        effect->peer != r->scope.coordinator ||
        !core_owner_report_decode(effect->body, effect->size, &report) ||
        report.operation.value != r->id.value || report.object != effect->object ||
        report.object < first_object(r) || report.object >= r->scope.object_count ||
        report.owner != s->admission->local_device || !(report.facts & CORE_OBJECT_CLEANED) ||
        r->local_facts[report.object] != report.facts)
        return;
    r->owner_delivered |= UINT32_C(1) << report.object;
}

bool core_edge_activate(core_edge_service *s, uint64_t sender, const uint8_t *body, size_t size)
{
    core_cbor_reader rd = {.data = body, .size = size};
    uint64_t count, version, kind, object;
    core_op_id id;
    if (body == NULL || !core_cbor_array(&rd, &count) || count != 4 ||
        !core_cbor_uint(&rd, &version) || version != CORE_SCHEMA_VERSION ||
        !core_cbor_uint(&rd, &kind) || kind != CORE_ACTIVATE_KIND ||
        !core_cbor_uint(&rd, &id.value) || !core_cbor_uint(&rd, &object) ||
        object >= CORE_REPORT_MAX_OBJECTS || rd.offset != size)
        return false;
    core_admission_record *r = find(s, id);
    if (!core_edge_supported(r) || !r->endpoint_only || removal(r) ||
        r->scope.coordinator != sender || object < first_object(r) ||
        object >= r->scope.object_count || r->cleanup_selected)
        return false;
    const size_t index = (size_t)object - first_object(r), c = index / 2;
    if (!known(r, c) || (r->channel_cleanup & (1u << c)) ||
        owner(r, (uint8_t)object) != s->admission->local_device)
        return false;
    return core_endpoint_activate(s->endpoints, &r->scope.bindings[c], index % 2 == 0);
}

bool core_edge_owner(core_edge_service *s, uint64_t sender, const uint8_t *body, size_t size,
                     uint64_t now)
{
    core_owner_report report;
    if (!core_owner_report_decode(body, size, &report) || report.owner != sender)
        return false;
    core_admission_record *r = find(s, report.operation);
    if (!core_edge_supported(r) || r->endpoint_only)
        return false;
    const core_report_result result = observation_result(s, r, sender, report.object, report.facts);
    if (result == CORE_REPORT_INVALID || result == CORE_REPORT_CONFLICT ||
        result == CORE_REPORT_EXHAUSTED)
        return false;
    core_edge_step(s, now);
    return observe(s, r, sender, report.object, report.facts, now);
}

bool core_edge_reply(core_edge_service *s, const core_edge_effect *effect, uint64_t sender,
                     const uint8_t *body, size_t size, uint64_t now)
{
    const core_admission_record *r = core_admission_find(s->admission, effect->operation);
    core_owner_report report;
    return r != NULL && r->incarnation == effect->incarnation &&
           r - s->admission->records == effect->record_slot && sender == effect->peer &&
           core_owner_report_decode(body, size, &report) &&
           report.operation.value == effect->operation.value && report.object == effect->object &&
           core_edge_owner(s, sender, body, size, now);
}

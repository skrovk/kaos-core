#include "admission_fixture.h"
#include "kaos_edge.h"
#include "kaos_control.h"

typedef struct {
    core_admission admission;
    core_endpoint_service endpoints;
    core_edge_service edges;
    core_control control;
} device;

static void device_init(device *d, uint64_t owner)
{
    memset(d, 0, sizeof(*d));
    fixture_pool(&d->admission, owner);
    core_endpoint_init(&d->endpoints, &d->admission, NULL, NULL);
    d->edges.admission = &d->admission;
    d->edges.endpoints = &d->endpoints;
    d->control.admission = &d->admission;
    d->control.edges = &d->edges;
}

static void node(device *d, uint64_t id, uint64_t logical)
{
    fixture_request request = fixture_node(id, logical);
    const core_admission_record *r;
    CHECK(fixture_submit(&d->admission, &request, &r) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&d->admission, (core_instance_id){id}));
}

static fixture_request edge_request(uint64_t destination)
{
    fixture_request r = {.id = {3}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    r.scope.bindings[0] = (core_binding){.creating_op = {3},
        .channel = fixture_channel(10, 20, 1, 2, 1, destination)};
    return r;
}

static core_channel_id channel_id(void)
{
    return (core_channel_id){10, 20, 7};
}

static const core_admission_record *submit(device *d, const fixture_request *request)
{
    const core_admission_record *r;
    CHECK(fixture_submit(&d->admission, request, &r) == CORE_ADMIT_ACCEPTED);
    return r;
}

static void deliver(device *to, uint64_t from, const core_edge_effect *effect, uint64_t now)
{
    static uint16_t message_id;
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
    CHECK(core_control_receive(&to->control, from, packet, size, now,
                                 response, sizeof(response), &response_size));
    core_coap_message reply;
    CHECK(core_coap_decode(response, response_size, &reply));
    CHECK(reply.code == CORE_COAP_CREATED || reply.code == CORE_COAP_CHANGED);
    core_edge_step(&to->edges, now);
}

static core_edge_effect next(device *d, uint64_t peer, uint64_t now, core_edge_effect_kind kind)
{
    core_edge_effect effect;
    CHECK(core_edge_next(&d->edges, peer, now, &effect));
    CHECK(effect.kind == kind && effect.peer == peer);
    return effect;
}

static uint8_t cleanup_exchange(device *to, const core_edge_effect *effect,
                                 uint16_t message_id, uint64_t now, device *coordinator)
{
    const char *path = core_edge_path(effect->kind);
    const uint8_t format = 60;
    core_coap_message request = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = message_id, .option_count = 2,
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)path, strlen(path)},
                    {CORE_COAP_CONTENT_FORMAT, &format, 1}},
        .payload = effect->body, .payload_size = effect->size};
    uint8_t packet[CORE_COAP_MAX_DATAGRAM], response[CORE_COAP_MAX_DATAGRAM];
    size_t size, response_size;
    CHECK(core_coap_encode(&request, packet, sizeof(packet), &size));
    CHECK(core_control_receive(&to->control, 1, packet, size, now,
                                 response, sizeof(response), &response_size));
    core_coap_message reply;
    core_owner_report report;
    CHECK(core_coap_decode(response, response_size, &reply));
    CHECK(reply.code == CORE_COAP_CREATED || reply.code == CORE_COAP_CHANGED);
    CHECK(core_owner_report_decode(reply.payload, reply.payload_size, &report));
    CHECK(report.operation.value == effect->operation.value && report.owner == 2 && report.object == 1);
    if (coordinator != NULL) {
        CHECK(core_edge_owner(&coordinator->edges, 2, reply.payload, reply.payload_size, now));
        core_edge_step(&coordinator->edges, now);
    }
    return report.facts;
}

static void remote_activation_and_removal(void)
{
    device source, destination;
    device_init(&source, 1);
    device_init(&destination, 2);
    node(&source, 1, 10);
    node(&destination, 2, 20);
    fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    core_edge_step(&source.edges, 11);
    CHECK(add->evidence.objects[0] == CORE_OBJECT_READY && add->evidence.objects[1] == 0);
    CHECK(add->evidence.history == 0);
    core_edge_effect prepare = next(&source, 2, 12, CORE_EDGE_PREPARE);
    deliver(&destination, 1, &prepare, 13);
    core_edge_effect ready = next(&destination, 1, 14, CORE_EDGE_OWNER);
    deliver(&source, 2, &ready, 15);
    CHECK(add->evidence.history == CORE_HISTORY_COMMIT);
    core_edge_effect activate = next(&source, 2, 16, CORE_EDGE_ACTIVATE);
    CHECK(!core_edge_activate(&destination.edges, 3, activate.body, activate.size));
    deliver(&destination, 1, &activate, 17);
    char payload = 'a', received;
    size_t written = 0;
    CHECK(core_channel_get(&destination.endpoints, (core_instance_id){2}, channel_id(),
                            &received, 1, &written) == CORE_DATA_EMPTY);
    CHECK(core_channel_put(&source.endpoints, (core_instance_id){1}, channel_id(),
                            &payload, 1) == CORE_DATA_DISABLED);
    core_edge_effect active = next(&destination, 1, 18, CORE_EDGE_OWNER);
    deliver(&source, 2, &active, 19);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_channel_put(&source.endpoints, (core_instance_id){1}, channel_id(),
                            &payload, 1) == CORE_DATA_OK);
    core_data_frame frame;
    uint64_t peer;
    CHECK(core_endpoint_next(&source.endpoints, &peer, &frame) && peer == 2);
    CHECK(core_endpoint_receive(&destination.endpoints, 1, &frame) == CORE_DATA_OK);
    CHECK(core_channel_get(&destination.endpoints, (core_instance_id){2}, channel_id(),
                            &received, 1, &written) == CORE_DATA_OK && received == payload && written == 1);
    request.id.value = 4;
    request.scope.kind = CORE_REQUEST_REMOVE_EDGE;
    request.scope.allowance_us = 100;
    const core_admission_record *remove = submit(&source, &request);
    core_edge_step(&source.edges, 20);
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_PENDING);
    core_edge_effect close = next(&source, 2, 21, CORE_EDGE_PREPARE);
    deliver(&destination, 1, &close, 22);
    /* Creator and removal have independent report streams. */
    for (unsigned i = 0; i < 2; ++i) {
        core_edge_effect cleaned = next(&destination, 1, 23 + i, CORE_EDGE_OWNER);
        deliver(&source, 2, &cleaned, 25 + i);
    }
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(source.admission.nodes[0].established && destination.admission.nodes[0].established);
    CHECK(source.admission.reserved_bytes == source.admission.node_bytes);
    CHECK(destination.admission.reserved_bytes == destination.admission.node_bytes);
    CHECK(core_endpoint_receive(&destination.endpoints, 1, &frame) == CORE_DATA_DISABLED);
}

static void cancel_before_prepare_and_late_evidence(void)
{
    device source, destination;
    device_init(&source, 1);
    device_init(&destination, 2);
    node(&source, 1, 10);
    node(&destination, 2, 20);
    fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    core_edge_effect lost_prepare = next(&source, 2, 11, CORE_EDGE_PREPARE);
    core_edge_step(&source.edges, 110); /* equality expires */
    CHECK(add->evidence.history & CORE_HISTORY_CLEANUP);
    CHECK(!core_edge_effect_current(&source.edges, &lost_prepare, 110));
    core_edge_effect cancel = next(&source, 2, 111, CORE_EDGE_CANCEL);
    core_edge_step(&source.edges, 160);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_UNRESOLVED);
    core_edge_effect none;
    CHECK(!core_edge_next(&source.edges, 2, 161, &none));
    deliver(&destination, 1, &cancel, 170); /* already-issued traffic may arrive late */
    core_edge_effect cleaned = next(&destination, 1, 171, CORE_EDGE_OWNER);
    deliver(&source, 2, &cleaned, 172);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(add->evidence.history & CORE_HISTORY_CUTOFF);
    const core_admission_record *r = NULL;
    CHECK(core_endpoint_submit(&destination.admission, 1, lost_prepare.body, lost_prepare.size,
                                 173, &r) == CORE_ADMIT_DUPLICATE);
    core_edge_step(&destination.edges, 174);
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i)
        CHECK(!destination.admission.endpoints[i].occupied);
    CHECK(source.admission.nodes[0].established && destination.admission.nodes[0].established);
}

static void local_access_holds_cleanup(void)
{
    device local;
    device_init(&local, 1);
    node(&local, 1, 10);
    node(&local, 2, 20);
    fixture_request request = edge_request(1);
    const core_admission_record *add = submit(&local, &request);
    core_edge_step(&local.edges, 11);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_SUCCEEDED);
    core_access access;
    core_reserved_endpoint *held = &local.admission.endpoints[1];
    CHECK(core_access_acquire(&held->lifetime, &access));
    request.id.value = 4;
    request.scope.kind = CORE_REQUEST_REMOVE_EDGE;
    request.scope.allowance_us = 30;
    const core_admission_record *remove = submit(&local, &request);
    core_edge_step(&local.edges, 12);
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_PENDING);
    CHECK(held->occupied);
    core_edge_step(&local.edges, 40);
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_UNRESOLVED);
    CHECK(core_access_release(&held->lifetime, access));
    core_edge_step(&local.edges, 41);
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(remove->evidence.history & CORE_HISTORY_CUTOFF);
    CHECK(local.admission.reserved_bytes == 2 * local.admission.node_bytes);
    CHECK(local.admission.nodes[0].established && local.admission.nodes[1].established);
}

static void reports_need_issued_actions(void)
{
    device source;
    device_init(&source, 1);
    node(&source, 1, 10);
    const fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    core_edge_step(&source.edges, 11);
    core_owner_report report = {.operation = {3}, .owner = 2, .object = 1, .facts = CORE_OBJECT_READY};
    uint8_t bytes[CORE_REPORT_BYTES];
    size_t size;
    CHECK(core_owner_report_encode(&report, bytes, sizeof(bytes), &size));
    CHECK(!core_edge_owner(&source.edges, 2, bytes, size, 12));
    (void)next(&source, 2, 13, CORE_EDGE_PREPARE);
    CHECK(!core_edge_owner(&source.edges, 3, bytes, size, 14));
    CHECK(core_edge_owner(&source.edges, 2, bytes, size, 15));
    report.facts |= CORE_OBJECT_ACTIVE;
    CHECK(core_owner_report_encode(&report, bytes, sizeof(bytes), &size));
    CHECK(!core_edge_owner(&source.edges, 2, bytes, size, 16));
    report.facts = CORE_OBJECT_FAILED; /* incomparable with accepted READY */
    CHECK(core_owner_report_encode(&report, bytes, sizeof(bytes), &size));
    const core_report before = add->evidence;
    CHECK(!core_edge_owner(&source.edges, 2, bytes, size, 17));
    CHECK(memcmp(&before, &add->evidence, sizeof(before)) == 0);
    CHECK(!add->cleanup_selected);
}

static void active_at_deadline_cannot_succeed(void)
{
    device source, destination;
    device_init(&source, 1);
    device_init(&destination, 2);
    node(&source, 1, 10);
    node(&destination, 2, 20);
    fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    core_edge_effect prepare = next(&source, 2, 11, CORE_EDGE_PREPARE);
    deliver(&destination, 1, &prepare, 12);
    core_edge_effect ready = next(&destination, 1, 13, CORE_EDGE_OWNER);
    deliver(&source, 2, &ready, 14);
    core_edge_effect activate = next(&source, 2, 15, CORE_EDGE_ACTIVATE);
    deliver(&destination, 1, &activate, 16);
    core_edge_effect active = next(&destination, 1, 17, CORE_EDGE_OWNER);
    deliver(&source, 2, &active, 110);
    CHECK(add->evidence.history & CORE_HISTORY_CLEANUP);
    CHECK(!(add->evidence.history & CORE_HISTORY_SUCCESS));
    CHECK(!(add->evidence.objects[0] & CORE_OBJECT_ACTIVE));
    core_edge_effect cancel = next(&source, 2, 111, CORE_EDGE_CANCEL);
    CHECK(cancel.deadline_us == 160);
    deliver(&destination, 1, &cancel, 112);
    core_edge_effect cleaned = next(&destination, 1, 113, CORE_EDGE_OWNER);
    deliver(&source, 2, &cleaned, 159);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(!(add->evidence.history & CORE_HISTORY_CUTOFF));
}

static void pending_removal_keeps_two_outcomes(void)
{
    device source, destination;
    device_init(&source, 1);
    device_init(&destination, 2);
    node(&source, 1, 10);
    node(&destination, 2, 20);
    fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    core_edge_effect prepare = next(&source, 2, 11, CORE_EDGE_PREPARE);
    request.id.value = 4;
    request.scope.kind = CORE_REQUEST_REMOVE_EDGE;
    request.scope.allowance_us = 100;
    const core_admission_record *remove = submit(&source, &request);
    core_edge_step(&source.edges, 12);
    CHECK(add->cleanup_selected && remove->cleanup_selected);
    CHECK(!core_edge_effect_current(&source.edges, &prepare, 12));
    for (unsigned i = 0; i < 2; ++i) {
        core_edge_effect close;
        CHECK(core_edge_next(&source.edges, 2, 13 + i, &close));
        CHECK(close.kind == CORE_EDGE_CANCEL || close.kind == CORE_EDGE_PREPARE);
        deliver(&destination, 1, &close, 15 + i);
    }
    for (unsigned i = 0; i < 2; ++i) {
        core_edge_effect cleaned = next(&destination, 1, 17 + i, CORE_EDGE_OWNER);
        deliver(&source, 2, &cleaned, 19 + i);
    }
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(!(add->evidence.history & CORE_HISTORY_COMMIT));
    CHECK(source.admission.nodes[0].established && destination.admission.nodes[0].established);
}

static void unobserved_ready_can_close_and_reports_do_not_restart(void)
{
    device source, destination;
    device_init(&source, 1);
    device_init(&destination, 2);
    node(&source, 1, 10);
    node(&destination, 2, 20);
    const fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    core_edge_effect prepare = next(&source, 2, 11, CORE_EDGE_PREPARE);
    deliver(&destination, 1, &prepare, 12);
    core_edge_effect ready = next(&destination, 1, 13, CORE_EDGE_OWNER);
    core_edge_effect none;
    /* A transport timeout may exhaust this one exchange, but never makes a
     * second exchange with unchanged evidence eligible. */
    CHECK(!core_edge_next(&destination.edges, 1, 14, &none));
    core_edge_rejected(&destination.edges, &ready, 15);
    CHECK(!core_edge_next(&destination.edges, 1, 16, &none));
    CHECK(add->evidence.objects[1] == 0);
    core_edge_step(&source.edges, 110);
    core_edge_effect cancel = next(&source, 2, 111, CORE_EDGE_CANCEL);
    deliver(&destination, 1, &cancel, 112);
    core_edge_effect cleaned = next(&destination, 1, 113, CORE_EDGE_OWNER);
    core_owner_report report;
    CHECK(core_owner_report_decode(cleaned.body, cleaned.size, &report));
    CHECK(report.facts == (CORE_OBJECT_READY | CORE_OBJECT_CLEANUP |
                            CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED));
    deliver(&source, 2, &cleaned, 114);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    /* A delayed old progress report cannot regress closure or begin work. */
    deliver(&source, 2, &ready, 115);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(!core_edge_next(&destination.edges, 1, cleaned.deadline_us, &none));
    CHECK(!core_edge_next(&source.edges, 2, 200, &none));
}

static void successful_node_exit_is_local_reclamation(void)
{
    device source, destination;
    device_init(&source, 1);
    device_init(&destination, 2);
    node(&source, 1, 10);
    node(&destination, 2, 20);
    const fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    core_edge_effect prepare = next(&source, 2, 11, CORE_EDGE_PREPARE);
    deliver(&destination, 1, &prepare, 12);
    core_edge_effect ready = next(&destination, 1, 13, CORE_EDGE_OWNER);
    deliver(&source, 2, &ready, 14);
    core_edge_effect activate = next(&source, 2, 15, CORE_EDGE_ACTIVATE);
    deliver(&destination, 1, &activate, 16);
    core_edge_effect active = next(&destination, 1, 17, CORE_EDGE_OWNER);
    deliver(&source, 2, &active, 18);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_SUCCEEDED);
    core_lifetime_close(&source.admission.nodes[0].lifetime);
    core_edge_step(&source.edges, 19);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(add->evidence.objects[0] & CORE_OBJECT_CLEANED);
    CHECK(!add->cleanup_selected);
    CHECK(!(add->evidence.history & CORE_HISTORY_CLEANUP));
    CHECK(source.admission.reserved_bytes == source.admission.node_bytes);
    CHECK(destination.admission.nodes[0].established);
    char byte;
    size_t written;
    CHECK(core_channel_get(&destination.endpoints, (core_instance_id){2}, channel_id(),
                            &byte, 1, &written) == CORE_DATA_EMPTY);
    core_edge_effect none;
    CHECK(!core_edge_next(&source.edges, 2, 20, &none));
    CHECK(core_edge_next_deadline(&source.edges) == UINT64_MAX);
}

static void owner_report_operation_is_not_binding_creation(void)
{
    device local;
    device_init(&local, 1);
    node(&local, 1, 10);
    node(&local, 2, 20);
    fixture_request request = edge_request(1);
    const core_admission_record *add = submit(&local, &request);
    core_edge_step(&local.edges, 11);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_SUCCEEDED);
    core_access access;
    core_reserved_endpoint *held = &local.admission.endpoints[1];
    CHECK(core_access_acquire(&held->lifetime, &access));
    request.id.value = 4;
    request.scope.kind = CORE_REQUEST_REMOVE_EDGE;
    request.scope.allowance_us = 100;
    const core_admission_record *remove = submit(&local, &request);
    core_edge_step(&local.edges, 12);
    const core_owner_report wrong = {.operation = {4}, .owner = 1, .object = 1,
        .facts = CORE_OBJECT_READY | CORE_OBJECT_ACTIVE | CORE_OBJECT_CLEANUP};
    uint8_t body[CORE_REPORT_BYTES];
    size_t size;
    CHECK(core_owner_report_encode(&wrong, body, sizeof(body), &size));
    const core_report before = remove->evidence;
    CHECK(!core_edge_owner(&local.edges, 1, body, size, 13));
    CHECK(memcmp(&before, &remove->evidence, sizeof(before)) == 0);
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_PENDING);
    CHECK(core_access_release(&held->lifetime, access));
    core_edge_step(&local.edges, 14);
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_SUCCEEDED);
}

static void conflicting_report_has_no_transition_side_effect(void)
{
    device source;
    device_init(&source, 1);
    node(&source, 1, 10);
    const fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    (void)next(&source, 2, 11, CORE_EDGE_PREPARE);
    core_owner_report report = {.operation = {3}, .owner = 2, .object = 1,
        .facts = CORE_OBJECT_READY};
    uint8_t body[CORE_REPORT_BYTES];
    size_t size;
    CHECK(core_owner_report_encode(&report, body, sizeof(body), &size));
    CHECK(core_edge_owner(&source.edges, 2, body, size, 12));
    CHECK(add->evidence.history == 0); /* next engine step may commit */
    const core_report before = add->evidence;
    report.facts = CORE_OBJECT_FAILED;
    CHECK(core_owner_report_encode(&report, body, sizeof(body), &size));
    CHECK(!core_edge_owner(&source.edges, 2, body, size, 13));
    CHECK(memcmp(&before, &add->evidence, sizeof(before)) == 0);
    CHECK(!add->cleanup_selected);
    core_edge_step(&source.edges, 14);
    CHECK(add->evidence.history == CORE_HISTORY_COMMIT);
}

static void pending_receiver_exit_cancels_authorized_edge(void)
{
    device source, destination;
    device_init(&source, 1);
    device_init(&destination, 2);
    node(&source, 1, 10);
    node(&destination, 2, 20);
    const fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    core_edge_effect prepare = next(&source, 2, 11, CORE_EDGE_PREPARE);
    deliver(&destination, 1, &prepare, 12);
    core_edge_effect ready = next(&destination, 1, 13, CORE_EDGE_OWNER);
    deliver(&source, 2, &ready, 14);
    core_edge_effect activate = next(&source, 2, 15, CORE_EDGE_ACTIVATE);
    core_lifetime_close(&destination.admission.nodes[0].lifetime);
    core_edge_step(&destination.edges, 16);
    CHECK(!core_edge_activate(&destination.edges, 1, activate.body, activate.size));
    core_edge_effect failed = next(&destination, 1, 17, CORE_EDGE_OWNER);
    deliver(&source, 2, &failed, 18);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(add->evidence.history & CORE_HISTORY_COMMIT);
    CHECK(add->evidence.history & CORE_HISTORY_CLEANUP);
    CHECK(!(add->evidence.objects[0] & CORE_OBJECT_ACTIVE));
    CHECK(!core_edge_effect_current(&source.edges, &activate, 19));
    CHECK(source.admission.nodes[0].established);
    CHECK(!source.admission.nodes[0].lifetime.closing);
    CHECK(source.admission.reserved_bytes == source.admission.node_bytes);
    CHECK(destination.admission.reserved_bytes == destination.admission.node_bytes);
}

static void duplicate_cancel_returns_retained_closure(void)
{
    device source, destination;
    device_init(&source, 1);
    device_init(&destination, 2);
    node(&source, 1, 10);
    node(&destination, 2, 20);
    const fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    (void)next(&source, 2, 11, CORE_EDGE_PREPARE); /* PREPARE is lost */
    core_edge_step(&source.edges, 110);
    core_edge_effect cancel = next(&source, 2, 111, CORE_EDGE_CANCEL);
    CHECK(cleanup_exchange(&destination, &cancel, 65000, 112, NULL) & CORE_OBJECT_CLEANED);
    (void)next(&destination, 1, 113, CORE_EDGE_OWNER); /* reply and push report lost */
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_PENDING);
    /* Retransmit the SAME cleanup exchange; the reply repeats retained facts. */
    CHECK(cleanup_exchange(&destination, &cancel, 65000, 114, &source) & CORE_OBJECT_CLEANED);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    core_edge_effect none;
    CHECK(!core_edge_next(&destination.edges, 1, 115, &none));
}

static void duplicate_removal_returns_later_closure(void)
{
    device source, destination;
    device_init(&source, 1);
    device_init(&destination, 2);
    node(&source, 1, 10);
    node(&destination, 2, 20);
    fixture_request request = edge_request(2);
    const core_admission_record *add = submit(&source, &request);
    core_edge_effect prepare = next(&source, 2, 11, CORE_EDGE_PREPARE);
    deliver(&destination, 1, &prepare, 12);
    core_edge_effect ready = next(&destination, 1, 13, CORE_EDGE_OWNER);
    deliver(&source, 2, &ready, 14);
    core_edge_effect activate = next(&source, 2, 15, CORE_EDGE_ACTIVATE);
    deliver(&destination, 1, &activate, 16);
    core_edge_effect active = next(&destination, 1, 17, CORE_EDGE_OWNER);
    deliver(&source, 2, &active, 18);
    core_access access;
    core_reserved_endpoint *held = &destination.admission.endpoints[0];
    CHECK(core_access_acquire(&held->lifetime, &access));
    request.id.value = 4;
    request.scope.kind = CORE_REQUEST_REMOVE_EDGE;
    request.scope.allowance_us = 100;
    uint8_t body[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(&request, body);
    const core_admission_record *remove;
    CHECK(core_request_submit(&source.admission, 99, body, size, 20, &remove) == CORE_ADMIT_ACCEPTED);
    core_edge_effect cleanup = next(&source, 2, 21, CORE_EDGE_PREPARE);
    CHECK(cleanup_exchange(&destination, &cleanup, 65001, 22, NULL) == CORE_OBJECT_CLEANUP);
    CHECK(core_access_release(&held->lifetime, access));
    core_edge_step(&destination.edges, 23);
    core_edge_effect lost;
    unsigned count = 0;
    while (core_edge_next(&destination.edges, 1, 24, &lost)) {
        CHECK(lost.kind == CORE_EDGE_OWNER && ++count <= 2);
    }
    CHECK(count == 2); /* creator and removal closure reports both lost */
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_PENDING);
    /* Replaying an original cached CLEANUP-only response would fail here. */
    CHECK(cleanup_exchange(&destination, &cleanup, 65001, 25, &source) & CORE_OBJECT_CLEANED);
    CHECK(core_report_outcome(&remove->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_report_outcome(&add->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(!core_edge_next(&destination.edges, 1, 26, &lost));
}

static void owner_report_deadline_failure_preserves_pending_evidence(void)
{
    device destination;
    device_init(&destination, 2);
    node(&destination, 2, 20);
    const fixture_request request = edge_request(2);
    uint8_t bytes[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(&request, bytes);
    const core_admission_record *record;
    CHECK(core_endpoint_submit(&destination.admission, 1, bytes, size, 10, &record) ==
          CORE_ADMIT_ACCEPTED);
    core_edge_step(&destination.edges, 11);
    CHECK(record->local_facts[1] & CORE_OBJECT_READY);
    const size_t slot = (size_t)(record - destination.admission.records);
    const core_edge_execution before = destination.edges.executions[slot];
    const size_t cursor = destination.edges.next_record;
    core_edge_effect effect;
    /* Encoding fits, but the exchange deadline cannot be represented. The
     * owner report must remain pending instead of being marked emitted. */
    CHECK(!core_edge_next(&destination.edges, 1, UINT64_MAX, &effect));
    CHECK(memcmp(&before, &destination.edges.executions[slot], sizeof(before)) == 0);
    CHECK(destination.edges.next_record == cursor);
}

int main(void)
{
    remote_activation_and_removal();
    cancel_before_prepare_and_late_evidence();
    local_access_holds_cleanup();
    reports_need_issued_actions();
    active_at_deadline_cannot_succeed();
    pending_removal_keeps_two_outcomes();
    unobserved_ready_can_close_and_reports_do_not_restart();
    successful_node_exit_is_local_reclamation();
    owner_report_operation_is_not_binding_creation();
    conflicting_report_has_no_transition_side_effect();
    pending_receiver_exit_cancels_authorized_edge();
    duplicate_cancel_returns_retained_closure();
    duplicate_removal_returns_later_closure();
    owner_report_deadline_failure_preserves_pending_evidence();
    puts("edges: receiver-first local/remote activation, scoped cleanup, cutoff and late evidence passed");
    return 0;
}

#include "admission_fixture.h"
#include "kaos_control.h"

typedef struct {
    core_admission admission;
    core_endpoint_service endpoints;
    core_edge_service edges;
    core_control control;
} device;

static void init_peers(device *d, uint64_t owner, uint64_t authority,
                        const uint64_t *peers, size_t peer_count)
{
    memset(d, 0, sizeof(*d));
    CHECK(core_admission_init(&d->admission, 4096, 2 * 4096 + 8 * CORE_ENDPOINT_BYTES));
    CHECK(core_admission_configure(&d->admission, owner, authority, peers, peer_count));
    core_endpoint_init(&d->endpoints, &d->admission, NULL, NULL);
    d->edges.admission = &d->admission;
    d->edges.endpoints = &d->endpoints;
    d->control.admission = &d->admission;
    d->control.edges = &d->edges;
}

static void init(device *d, uint64_t owner)
{
    const uint64_t peers[] = {1, 2, 3};
    init_peers(d, owner, 99, peers, 3);
}

static void established(device *d, uint64_t id, uint64_t logical)
{
    const fixture_request request = fixture_node(id, logical);
    const core_admission_record *r;
    CHECK(fixture_submit(&d->admission, &request, &r) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&d->admission, (core_instance_id){id}));
}

static uint8_t exchange(device *owner, uint64_t sender, const core_edge_effect *effect,
                        const char *path, uint16_t mid, uint64_t now, core_owner_report *report)
{
    const uint8_t format = 60;
    const core_coap_message request = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = mid, .option_count = 2,
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)path, strlen(path)},
                    {CORE_COAP_CONTENT_FORMAT, &format, 1}},
        .payload = effect->body, .payload_size = effect->size};
    uint8_t packet[CORE_COAP_MAX_DATAGRAM], response[CORE_COAP_MAX_DATAGRAM];
    size_t size, response_size;
    CHECK(core_coap_encode(&request, packet, sizeof(packet), &size));
    CHECK(core_control_receive(&owner->control, sender, packet, size, now,
        response, sizeof(response), &response_size));
    core_coap_message reply;
    CHECK(core_coap_decode(response, response_size, &reply));
    if (report != NULL) CHECK(core_owner_report_decode(reply.payload, reply.payload_size, report));
    else CHECK(reply.payload_size == 0);
    return reply.code;
}

static void accept(device *coordinator, const core_owner_report *report, uint64_t now)
{
    uint8_t encoded[CORE_REPORT_BYTES];
    size_t size;
    CHECK(core_owner_report_encode(report, encoded, sizeof(encoded), &size));
    CHECK(core_edge_owner(&coordinator->edges, report->owner, encoded, size, now));
}

static void incoming_cleanup_snapshot_advances_on_same_exchange(void)
{
    device coordinator, source;
    init(&coordinator, 1);
    init(&source, 2);
    established(&coordinator, 2, 20);
    established(&source, 1, 10);
    fixture_request addition = {.id = {3}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    addition.scope.bindings[0] = (core_binding){.creating_op = {3},
        .channel = fixture_channel(10, 20, 1, 2, 2, 1)};
    const core_admission_record *creator;
    CHECK(fixture_submit(&source.admission, &addition, &creator) == CORE_ADMIT_ACCEPTED);
    core_edge_step(&source.edges, 11);
    core_access access;
    CHECK(core_access_acquire(&source.admission.endpoints[0].lifetime, &access));
    const fixture_request removal = {.id = {10}, .scope = {.kind = CORE_REQUEST_REMOVE_NODE,
        .logical_node_id = 20, .instance = {2}, .allowance_us = 100, .channel_count = 1,
        .bindings = {addition.scope.bindings[0]}}};
    const core_admission_record *observer;
    CHECK(fixture_submit(&coordinator.admission, &removal, &observer) == CORE_ADMIT_ACCEPTED);
    core_edge_effect effect;
    CHECK(core_edge_next(&coordinator.edges, 2, 12, &effect));
    CHECK(effect.kind == CORE_EDGE_PREPARE && effect.object == 1 && effect.deadline_us == 110);

    /* Compact cleanup still runs when normal response-cache slots are full. */
    for (unsigned i = 0; i < CORE_COAP_CACHE_SIZE; ++i) {
        core_coap_cached_response *entry;
        CHECK(core_coap_cache_reserve(&source.control.cache, 1, (uint16_t)i, 12, &entry) ==
            CORE_COAP_CACHE_NEW);
    }
    core_owner_report report;
    CHECK(exchange(&source, 1, &effect, "prepare", 100, 13, &report) == CORE_COAP_CHANGED);
    CHECK(report.operation.value == 10 && report.owner == 2 && report.object == 1);
    CHECK(report.facts == CORE_OBJECT_CLEANUP);
    accept(&coordinator, &report, 13);
    CHECK(!(observer->evidence.objects[1] & CORE_OBJECT_CLEANED));
    CHECK(source.admission.endpoints[0].occupied);
    CHECK(core_access_release(&source.admission.endpoints[0].lifetime, access));

    /* The identical MID returns current closure, never its earlier CLEANUP. */
    CHECK(exchange(&source, 1, &effect, "prepare", 100, 14, &report) == CORE_COAP_CHANGED);
    CHECK(report.facts == (CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED));
    accept(&coordinator, &report, 14);
    CHECK(observer->evidence.objects[1] & CORE_OBJECT_CLEANED);
    CHECK(!source.admission.endpoints[0].occupied);
    CHECK(source.admission.nodes[0].established && !source.admission.nodes[0].lifetime.closing);
    CHECK(creator->evidence.history & CORE_HISTORY_CLEANUP);
    CHECK(!(creator->evidence.objects[1] & CORE_OBJECT_CLEANED));
    const uint8_t revision = observer->evidence.revision;
    CHECK(exchange(&source, 1, &effect, "prepare", 100, 15, &report) == CORE_COAP_CHANGED);
    accept(&coordinator, &report, 15);
    CHECK(observer->evidence.revision == revision && observer->cleanup_deadline_us == 110);
    CHECK(core_edge_effect_current(&coordinator.edges, &effect, 109));
    CHECK(!core_edge_effect_current(&coordinator.edges, &effect, 110));
}

static void maximum_width_and_highest_index(void)
{
    device coordinator, owner;
    const uint64_t local = UINT64_MAX - 10, remote = UINT64_MAX - 11, authority = UINT64_MAX - 12;
    const uint64_t peers[] = {local, remote};
    init_peers(&coordinator, local, authority, peers, 2);
    init_peers(&owner, remote, authority, peers, 2);
    fixture_request request = {.id = {UINT64_MAX}, .scope = {.kind = CORE_REQUEST_REMOVE_NODE,
        .logical_node_id = UINT64_MAX - 3, .instance = {UINT64_MAX - 1},
        .allowance_us = UINT64_MAX - 20, .channel_count = CORE_MAX_REQUEST_CHANNELS}};
    for (unsigned c = 0; c < CORE_MAX_REQUEST_CHANNELS; ++c) {
        core_binding *binding = &request.scope.bindings[c];
        binding->creating_op.value = UINT64_MAX - 2;
        binding->channel = fixture_channel(UINT64_MAX - 3, UINT64_MAX - 4,
            UINT64_MAX - 1, UINT64_MAX - 5, local, remote);
        binding->channel.service = UINT64_MAX - c;
        binding->channel.source_port = binding->channel.destination_port = UINT16_MAX;
        binding->channel.type = UINT16_MAX;
        binding->channel.payload_bytes = CORE_MAX_PAYLOAD_BYTES;
        binding->channel.queue_messages = CORE_MAX_QUEUE_MESSAGES;
        binding->channel.addition_allowance_us = UINT64_MAX;
    }
    uint8_t encoded[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(&request, encoded);
    const core_admission_record *observer;
    CHECK(core_request_submit(&coordinator.admission, authority, encoded, size, 10, &observer) ==
        CORE_ADMIT_ACCEPTED);
    for (unsigned c = 0; c < CORE_MAX_REQUEST_CHANNELS; ++c) {
        core_edge_effect effect;
        CHECK(core_edge_next(&coordinator.edges, remote, 11 + c, &effect));
        CHECK(effect.kind == CORE_EDGE_PREPARE && effect.object == 2 + 2 * c);
        CHECK(effect.size <= CORE_EDGE_MESSAGE_BYTES && effect.size > 100);
        CHECK(effect.deadline_us == UINT64_MAX - 10);
        core_owner_report report;
        CHECK(exchange(&owner, local, &effect, "prepare", (uint16_t)(100 + c), 20 + c, &report) ==
            CORE_COAP_CHANGED);
        CHECK(report.operation.value == UINT64_MAX && report.owner == remote);
        CHECK(report.object == effect.object && report.facts & CORE_OBJECT_CLEANED);
        accept(&coordinator, &report, 20 + c);
    }
    CHECK(observer->evidence.objects[CORE_REPORT_MAX_OBJECTS - 1] & CORE_OBJECT_CLEANED);
    CHECK(core_report_obligations(&observer->evidence) == 1);
    CHECK(owner.admission.reserved_bytes == 0 && coordinator.admission.reserved_bytes == 0);
}

static void invalid_commands_do_not_publish_cleanup(void)
{
    device coordinator, owner;
    init(&coordinator, 1);
    init(&owner, 2);
    fixture_request request = {.id = {10}, .scope = {.kind = CORE_REQUEST_REMOVE_NODE,
        .logical_node_id = 20, .instance = {2}, .allowance_us = 100, .channel_count = 1}};
    request.scope.bindings[0] = (core_binding){.creating_op = {3},
        .channel = fixture_channel(10, 20, 1, 2, 2, 1)};
    const core_admission_record *observer;
    CHECK(fixture_submit(&coordinator.admission, &request, &observer) == CORE_ADMIT_ACCEPTED);
    core_edge_effect effect;
    CHECK(core_edge_next(&coordinator.edges, 2, 11, &effect));
    const core_admission before = owner.admission;
    CHECK(exchange(&owner, 1, &effect, "cancel", 100, 12, NULL) == CORE_COAP_BAD_REQUEST);
    CHECK(memcmp(&before, &owner.admission, sizeof(before)) == 0);
    CHECK(exchange(&owner, 3, &effect, "prepare", 101, 13, NULL) == CORE_COAP_BAD_REQUEST);
    CHECK(memcmp(&before, &owner.admission, sizeof(before)) == 0);
    --effect.size;
    CHECK(exchange(&owner, 1, &effect, "prepare", 102, 14, NULL) == CORE_COAP_BAD_REQUEST);
    CHECK(memcmp(&before, &owner.admission, sizeof(before)) == 0);
    ++effect.size;
    effect.body[0] = 0x85; /* Wrong array length, with otherwise valid fields. */
    CHECK(exchange(&owner, 1, &effect, "prepare", 103, 15, NULL) == CORE_COAP_BAD_REQUEST);
    CHECK(memcmp(&before, &owner.admission, sizeof(before)) == 0);
}

int main(void)
{
    incoming_cleanup_snapshot_advances_on_same_exchange();
    maximum_width_and_highest_index();
    invalid_commands_do_not_publish_cleanup();
    puts("P6 control checks passed");
    return 0;
}

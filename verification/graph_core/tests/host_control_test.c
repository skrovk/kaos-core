#include "admission_fixture.h"
#include "kaos_control.h"

static void late_closure(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    core_endpoint_service endpoints;
    core_endpoint_init(&endpoints, &pool, NULL, NULL);
    core_edge_service edges = {.admission = &pool, .endpoints = &endpoints};
    core_control control = {.admission = &pool, .edges = &edges};
    core_control_peer peer = {.id = 2};
    core_coap_ids ids = {.next_mid = 1, .next_token = 1};
    const core_admission_record *record;
    fixture_request node = fixture_node(1, 10);
    CHECK(fixture_submit(&pool, &node, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){1}));
    fixture_request edge = {.id = {3}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    edge.scope.bindings[0] = (core_binding){.creating_op = {3},
        .channel = fixture_channel(10, 20, 1, 2, 1, 2)};
    CHECK(fixture_submit(&pool, &edge, &record) == CORE_ADMIT_ACCEPTED);
    uint8_t bytes[CORE_COAP_MAX_DATAGRAM];
    size_t size;
    CHECK(core_control_peer_next(&control, &peer, &ids, 20,
        CORE_COAP_ACK_TIMEOUT_US, bytes, sizeof(bytes), &size));
    CHECK(peer.effect.kind == CORE_EDGE_PREPARE);
    core_edge_step(&edges, 110);
    CHECK(core_control_peer_next(&control, &peer, &ids, 110,
        CORE_COAP_ACK_TIMEOUT_US, bytes, sizeof(bytes), &size));
    CHECK(peer.effect.kind == CORE_EDGE_CANCEL && peer.exchange.deadline_us == 160);
    core_edge_step(&edges, 160);
    CHECK(!core_control_peer_next(&control, &peer, &ids, 160,
        CORE_COAP_ACK_TIMEOUT_US, bytes, sizeof(bytes), &size));
    CHECK(core_report_outcome(&record->evidence) == CORE_OUTCOME_UNRESOLVED);
    CHECK(!peer.exchange.active);
    const core_owner_report closure = {.operation = {3}, .owner = 2, .object = 1,
        .facts = CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED};
    uint8_t body[CORE_REPORT_BYTES];
    CHECK(core_owner_report_encode(&closure, body, sizeof(body), &size));
    core_coap_message reply = {.type = CORE_COAP_ACK, .code = CORE_COAP_CHANGED,
        .message_id = peer.exchange.message_id, .token_size = peer.exchange.token_size,
        .payload = body, .payload_size = size};
    memcpy(reply.token, peer.exchange.token, reply.token_size);
    bool acknowledge;
    const uint8_t slot = peer.effect.record_slot;
    peer.effect.record_slot = (uint8_t)((slot + 1) % CORE_MAX_ADMISSION_RECORDS);
    core_control_peer_receive(&control, &peer, &reply, 161, &acknowledge);
    CHECK(core_report_outcome(&record->evidence) == CORE_OUTCOME_UNRESOLVED);
    peer.effect.record_slot = slot;
    ++peer.effect.incarnation;
    core_control_peer_receive(&control, &peer, &reply, 161, &acknowledge);
    CHECK(core_report_outcome(&record->evidence) == CORE_OUTCOME_UNRESOLVED);
    --peer.effect.incarnation;
    reply.token[0] ^= 1;
    core_control_peer_receive(&control, &peer, &reply, 161, &acknowledge);
    CHECK(core_report_outcome(&record->evidence) == CORE_OUTCOME_UNRESOLVED);
    reply.token[0] ^= 1;
    core_control_peer_receive(&control, &peer, &reply, 161, &acknowledge);
    CHECK(core_report_outcome(&record->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(!peer.exchange.active && record->cleanup_deadline_us == 160);
    CHECK(core_coap_exchange_deadline(&peer.exchange) == UINT64_MAX);
    const uint8_t revision = record->evidence.revision;
    core_control_peer_receive(&control, &peer, &reply, 162, &acknowledge);
    CHECK(record->evidence.revision == revision);
}
static void counter_exhaustion(void)
{
    core_coap_ids ids = {.next_mid = UINT16_MAX, .next_token = UINT64_MAX};
    core_coap_message message = {0};
    CHECK(core_coap_allocate_request(&ids, &message));
    CHECK(message.message_id == UINT16_MAX && message.token_size == 8);
    for (size_t i = 0; i < 8; ++i) CHECK(message.token[i] == 255);
    CHECK(!core_coap_allocate_request(&ids, &message));
    uint64_t token;
    CHECK(!core_coap_allocate_token(&ids, &token));
    CHECK(ids.next_mid == (uint32_t)UINT16_MAX + 1);
}
int main(void)
{
    late_closure();
    counter_exhaustion();
    puts("management exchange: late closure, stale effects and exhaustion passed");
}

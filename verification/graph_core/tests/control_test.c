#include "admission_fixture.h"
#include "kaos_control.h"

static uint8_t exchange(core_control *control, uint64_t peer, uint16_t mid,
                         const char *path, const uint8_t *body, size_t size,
                         const core_coap_option *extra, size_t extra_count,
                         uint8_t *response, size_t *response_size)
{
    const uint8_t format = 60;
    core_coap_message request = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = mid, .token_size = 2, .token = {0xab, 0xcd},
        .payload = body, .payload_size = size, .option_count = 2,
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)path, strlen(path)},
                    {CORE_COAP_CONTENT_FORMAT, &format, 1}}};
    for (size_t i = 0; i < extra_count; ++i) request.options[request.option_count++] = extra[i];
    uint8_t wire[CORE_COAP_MAX_DATAGRAM];
    size_t wire_size;
    CHECK(core_coap_encode(&request, wire, sizeof(wire), &wire_size));
    CHECK(core_control_receive(control, peer, wire, wire_size, 10,
                               response, CORE_COAP_MAX_DATAGRAM, response_size));
    core_coap_message reply;
    CHECK(core_coap_decode(response, *response_size, &reply));
    CHECK(reply.type == CORE_COAP_ACK && reply.message_id == mid);
    CHECK(reply.token_size == 2 && memcmp(reply.token, request.token, 2) == 0);
    return reply.code;
}

int main(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    core_control control = {.admission = &pool};
    fixture_request node = fixture_node(0, 1);
    uint8_t body[CORE_MAX_REQUEST_BYTES], response[CORE_COAP_MAX_DATAGRAM];
    size_t size = fixture_encode(&node, body), response_size;
    CHECK(exchange(&control, 98, 1, "graph", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_UNAUTHORIZED);
    CHECK(pool.reserved_bytes == 0);
    CHECK(exchange(&control, 99, 1, "graph", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_CREATED);
    const size_t reserved = pool.reserved_bytes;
    CHECK(exchange(&control, 99, 1, "graph", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_CREATED); /* lost ACK replay */
    CHECK(exchange(&control, 99, 2, "graph", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_CHANGED); /* logical discard */
    CHECK(pool.reserved_bytes == reserved);
    CHECK(exchange(&control, 99, 3, "graph", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_CHANGED);
    CHECK(exchange(&control, 99, 4, "graph", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_CHANGED);
    CHECK(exchange(&control, 99, 5, "graph", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_UNAVAILABLE);
    size = fixture_envelope(body, CORE_QUERY_KIND, 0, NULL, 0);
    CHECK(exchange(&control, 99, 6, "query", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_CONTENT);
    core_coap_message reply;
    core_report report;
    CHECK(core_coap_decode(response, response_size, &reply));
    CHECK(core_report_decode(reply.payload, reply.payload_size, 1, &report));
    CHECK(core_report_outcome(&report) == CORE_OUTCOME_PENDING && report.revision == 0);

    /* Multiple control requests can use the one decoder workspace in sequence.
     * Only complete bodies enter admission; the final ACK is retained after
     * workspace reset, and a resource switch cannot splice a partial request. */
    core_admission other;
    fixture_pool(&other, 1);
    core_control blocks = {.admission = &other};
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
        node = fixture_node(attempt, attempt + 1);
        size = fixture_encode(&node, body);
        for (size_t offset = 0; offset < size; offset += 16) {
            const size_t length = size - offset < 16 ? size - offset : 16;
            const bool more = offset + length < size;
            uint8_t block_bytes[4], size_bytes[4];
            const size_t bn = core_coap_uint((uint32_t)(offset | (more ? 8 : 0)), block_bytes);
            const size_t sn = core_coap_uint((uint32_t)size, size_bytes);
            const core_coap_option extra[] = {{CORE_COAP_BLOCK1, block_bytes, bn},
                {CORE_COAP_SIZE1, size_bytes, sn}, {CORE_COAP_REQUEST_TAG, &attempt, 1}};
            const uint16_t mid = (uint16_t)(100 + attempt * 20 + offset / 16);
            const uint8_t expected = more ? CORE_COAP_CONTINUE : CORE_COAP_CREATED;
            CHECK(exchange(&blocks, 99, mid, "graph", body + offset, length, extra, 3,
                           response, &response_size) == expected);
            CHECK(exchange(&blocks, 99, mid, "graph", body + offset, length, extra, 3,
                           response, &response_size) == expected);
            if (offset == 0) CHECK(other.reserved_bytes == (size_t)attempt * other.node_bytes);
        }
        CHECK(!blocks.assembly.active);
    }
    CHECK(other.reserved_bytes == 2 * other.node_bytes);

    /* Owner cancellation uses no transport response-cache capacity. A missing
     * endpoint still gets a retained cancellation fence before delayed ADD. */
    fixture_request add = fixture_node(40, 40);
    add.scope.kind = CORE_REQUEST_ADD_NODE;
    add.scope.channel_count = 1;
    add.scope.bindings[0].channel = fixture_channel(40, 41, 40, 41, 2, 1);
    const core_predicate_token always = {.kind = CORE_PREDICATE_TRUE};
    CHECK(core_predicate_init(&add.scope.predicate, &always, 1, 1));
    size = fixture_encode(&add, body);
    CHECK(exchange(&control, 2, 1000, "cancel", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_CREATED);
    CHECK(exchange(&control, 2, 1001, "cancel", body, size, NULL, 0, response,
                   &response_size) == CORE_COAP_CHANGED);
    CHECK(core_admission_find(&pool, (core_op_id){40})->cleanup_selected);
    puts("control: admission, lost ACK, logical duplicate, cache pressure, query and Block1 passed");
    return 0;
}

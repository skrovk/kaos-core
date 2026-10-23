#include "admission_fixture.h"
#include "kaos_control.h"
#include "kaos_node.h"

static uint8_t exchange(core_control *control, uint64_t sender, const char *path,
                        const uint8_t *body, size_t size, uint8_t *payload, size_t *length)
{
    static uint16_t mid;
    const uint8_t format = 60;
    const core_coap_message request = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = ++mid, .option_count = 2,
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)path, strlen(path)},
                    {CORE_COAP_CONTENT_FORMAT, &format, 1}}, .payload = body, .payload_size = size};
    uint8_t data[CORE_COAP_MAX_DATAGRAM], response[CORE_COAP_MAX_DATAGRAM];
    size_t written, received;
    CHECK(core_coap_encode(&request, data, sizeof(data), &written));
    CHECK(core_control_receive(control, sender, data, written, 12, response, sizeof(response), &received));
    core_coap_message reply;
    CHECK(core_coap_decode(response, received, &reply));
    CHECK(reply.payload_size <= CORE_REPORT_BYTES);
    if (reply.payload_size) memcpy(payload, reply.payload, reply.payload_size);
    *length = reply.payload_size;
    return reply.code;
}

static size_t ack(uint8_t *body, uint64_t operation, uint64_t coordinator, uint8_t revision)
{
    core_cbor_writer w = {.data = body, .size = CORE_REPORT_BYTES};
    core_cbor_write_array(&w, 5);
    core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
    core_cbor_write_uint(&w, CORE_STORAGE_ACK_KIND);
    core_cbor_write_uint(&w, operation);
    core_cbor_write_uint(&w, coordinator);
    core_cbor_write_uint(&w, revision);
    return w.offset;
}

static void missing(const uint8_t *payload, size_t length, uint64_t id, bool observations)
{
    core_cbor_reader r = {.data = payload, .size = length};
    uint64_t n;
    CHECK(core_cbor_array(&r, &n) && n == 5);
    CHECK(core_cbor_uint(&r, &n) && n == CORE_SCHEMA_VERSION);
    CHECK(core_cbor_uint(&r, &n) && n == CORE_NO_RESULT_KIND);
    CHECK(core_cbor_uint(&r, &n) && n == id);
    CHECK(core_cbor_uint(&r, &n) && n == 1);
    CHECK(core_cbor_array(&r, &n) && n == (observations ? 3 : 0));
    if (!observations) CHECK(r.offset == length);
}

int main(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    core_control control = {.admission = &pool, .collector = 0, .collector_enabled = true};
    const fixture_request request = fixture_node(100, 10);
    const core_admission_record *record;
    CHECK(fixture_submit(&pool, &request, &record) == CORE_ADMIT_ACCEPTED);
    uint8_t body[CORE_MAX_REQUEST_BYTES], payload[CORE_REPORT_BYTES];
    size_t length;
    size_t size = fixture_envelope(body, CORE_QUERY_KIND, 100, NULL, 0);
    CHECK(exchange(&control, 0, "query", body, size, payload, &length) == CORE_COAP_CONTENT);
    CHECK(exchange(&control, 2, "query", body, size, payload, &length) == CORE_COAP_UNAUTHORIZED);
    size = fixture_encode(&request, body);
    CHECK(exchange(&control, 0, "graph", body, size, payload, &length) == CORE_COAP_UNAUTHORIZED);
    /* ACKs work even when ordinary response cache is completely reserved. */
    for (size_t i = 0; i < CORE_COAP_CACHE_SIZE; ++i) {
        core_coap_cached_response *entry;
        CHECK(core_coap_cache_reserve(&control.cache, 99, (uint16_t)(1000 + i), 10, &entry) == CORE_COAP_CACHE_NEW);
    }
    size = ack(body, 100, 1, record->evidence.revision);
    CHECK(exchange(&control, 2, "stored", body, size, payload, &length) == CORE_COAP_UNAUTHORIZED);
    CHECK(core_admission_report_retained(record));
    for (size_t cut = 1; cut < size; ++cut)
        CHECK(exchange(&control, 0, "stored", body, cut, payload, &length) == CORE_COAP_BAD_REQUEST);
    CHECK(exchange(&control, 0, "stored", body, size, payload, &length) == CORE_COAP_CHANGED);
    CHECK(exchange(&control, 99, "stored", body, size, payload, &length) == CORE_COAP_CHANGED);
    core_admission_maintain(&pool);
    CHECK(record->node != NULL && !core_admission_report_retained(record));
    size = fixture_envelope(body, CORE_QUERY_KIND, 100, NULL, 0);
    CHECK(exchange(&control, 99, "query", body, size, payload, &length) == CORE_COAP_NOT_FOUND);
    missing(payload, length, 100, true);
    core_node_service service = {.admission = &pool};
    CHECK(core_admission_close_scope(&pool, record->id));
    core_node_step(&service, 20);
    CHECK(record->node == NULL && core_admission_report_retained(record));
    CHECK(exchange(&control, 0, "query", body, size, payload, &length) == CORE_COAP_CONTENT);
    core_report report;
    CHECK(core_report_decode(payload, length, 1, &report));
    CHECK(core_report_outcome(&report) == CORE_OUTCOME_FAILED_CLEAN);
    size = ack(body, 100, 1, report.revision);
    CHECK(exchange(&control, 99, "stored", body, size, payload, &length) == CORE_COAP_CHANGED);
    core_admission_maintain(&pool);
    size = fixture_envelope(body, CORE_QUERY_KIND, 100, NULL, 0);
    CHECK(exchange(&control, 99, "query", body, size, payload, &length) == CORE_COAP_NOT_FOUND);
    missing(payload, length, 100, false);
    size = fixture_envelope(body, CORE_QUERY_KIND, 999, NULL, 0);
    CHECK(exchange(&control, 0, "query", body, size, payload, &length) == CORE_COAP_NOT_FOUND);
    missing(payload, length, 999, false);
    puts("P7 control: designated recipients, exact ACK and missing/history distinction passed");
    return 0;
}

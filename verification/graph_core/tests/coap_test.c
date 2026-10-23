#include "kaos_coap.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t request(uint8_t *bytes, uint16_t message_id)
{
    const core_coap_message message = {
        .type = CORE_COAP_CON, .code = CORE_COAP_GET,
        .message_id = message_id, .token = {0xa0, 0x01}, .token_size = 2,
        .options = {{.number = CORE_COAP_URI_PATH,
                     .value = (const uint8_t *)"artifact", .size = 8}},
        .option_count = 1
    };
    size_t size;
    assert(core_coap_encode(&message, bytes, CORE_COAP_MAX_DATAGRAM, &size));
    return size;
}

static void codec(void)
{
    /* Independent RFC-style wire fixture, including zero-length Block2=0. */
    const uint8_t fixture[] = {0x42, 0x01, 0x12, 0x34, 0xa0, 0x01,
                               0xb8, 'a', 'r', 't', 'i', 'f', 'a', 'c', 't', 0xc0};
    core_coap_message decoded;
    assert(core_coap_decode(fixture, sizeof(fixture), &decoded));
    assert(decoded.message_id == 0x1234 && decoded.token_size == 2);
    assert(decoded.option_count == 2 && decoded.options[1].number == CORE_COAP_BLOCK2);
    uint32_t value = 99;
    assert(core_coap_option_uint(core_coap_find(&decoded, CORE_COAP_BLOCK2), &value));
    assert(value == 0);
    uint8_t encoded[CORE_COAP_MAX_DATAGRAM];
    size_t size;
    assert(core_coap_encode(&decoded, encoded, sizeof(encoded), &size));
    assert(size == sizeof(fixture) && memcmp(encoded, fixture, size) == 0);

    const uint8_t empty_ack[] = {0x60, 0, 0x12, 0x34};
    assert(core_coap_decode(empty_ack, sizeof(empty_ack), &decoded));
    const uint8_t empty_with_token[] = {0x61, 0, 0x12, 0x34, 0x01};
    const uint8_t invalid_marker[] = {0x40, 1, 0, 0, 0xff};
    const uint8_t invalid_delta[] = {0x40, 1, 0, 0, 0xf0};
    const uint8_t truncated_extension[] = {0x40, 1, 0, 0, 0xe0, 0};
    const uint8_t wrong_version[] = {0x00, 1, 0, 0};
    const uint8_t ack_request[] = {0x60, 1, 0, 0};
    const uint8_t bad_token[] = {0x49, 1, 0, 0};
    assert(!core_coap_decode(empty_with_token, sizeof(empty_with_token), &decoded));
    assert(!core_coap_decode(invalid_marker, sizeof(invalid_marker), &decoded));
    assert(!core_coap_decode(invalid_delta, sizeof(invalid_delta), &decoded));
    assert(!core_coap_decode(truncated_extension, sizeof(truncated_extension), &decoded));
    assert(!core_coap_decode(wrong_version, sizeof(wrong_version), &decoded));
    assert(!core_coap_decode(ack_request, sizeof(ack_request), &decoded));
    assert(!core_coap_decode(bad_token, sizeof(bad_token), &decoded));
    assert(!core_coap_decode(NULL, 0, &decoded));

    uint8_t long_option[270] = {0};
    const uint8_t payload[] = {1, 2, 3};
    core_coap_message extended = {
        .type = CORE_COAP_ACK, .code = CORE_COAP_CONTENT,
        .options = {{.number = 292, .value = long_option, .size = sizeof(long_option)}},
        .option_count = 1, .payload = payload, .payload_size = sizeof(payload)
    };
    assert(core_coap_encode(&extended, encoded, sizeof(encoded), &size));
    assert(core_coap_decode(encoded, size, &decoded));
    assert(decoded.options[0].number == 292 && decoded.options[0].size == 270);
    assert(decoded.payload_size == 3 && memcmp(decoded.payload, payload, 3) == 0);
    size_t unchanged = 123;
    assert(!core_coap_encode(&extended, encoded, 4, &unchanged) && unchanged == 123);
    for (size_t i = 0; i < 4; ++i) {
        assert(!core_coap_decode(encoded, i, &decoded));
    }
    uint8_t uint_bytes[4];
    const uint32_t values[] = {0, 1, 255, 256, 65535, 65536, UINT32_MAX};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        const size_t count = core_coap_uint(values[i], uint_bytes);
        const core_coap_option option = {.value = uint_bytes, .size = count};
        assert(core_coap_option_uint(&option, &value) && value == values[i]);
    }
}

static void exchanges(void)
{
    uint8_t bytes[CORE_COAP_MAX_DATAGRAM];
    const size_t size = request(bytes, 0x1234);
    core_coap_exchange exchange = {0};
    assert(!core_coap_exchange_start(&exchange, 7, bytes, size, 10, 100, 1));
    assert(core_coap_exchange_start(&exchange, 7, bytes, size, 10, UINT64_C(100000000),
                                    CORE_COAP_ACK_TIMEOUT_US));
    assert(!core_coap_exchange_start(&exchange, 7, bytes, size, 10, UINT64_C(100000000),
                                     CORE_COAP_ACK_TIMEOUT_US));
    const uint64_t retry_at[] = {2000010, 6000010, 14000010, 30000010};
    for (size_t i = 0; i < 4; ++i) {
        assert(core_coap_exchange_poll(&exchange, retry_at[i] - 1) == CORE_COAP_WAIT);
        assert(core_coap_exchange_poll(&exchange, retry_at[i]) == CORE_COAP_SEND);
        assert(exchange.request_size == size && memcmp(exchange.request, bytes, size) == 0);
    }
    assert(core_coap_exchange_poll(&exchange, UINT64_C(62000010)) == CORE_COAP_TIMED_OUT);
    assert(!exchange.active);

    assert(core_coap_exchange_start(&exchange, 7, bytes, size, 0, UINT64_C(10000000),
                                    CORE_COAP_ACK_TIMEOUT_US));
    core_coap_message response = {.type = CORE_COAP_ACK, .message_id = 0x1234};
    bool ack = true;
    assert(core_coap_exchange_receive(&exchange, 8, &response, 1, &ack) == CORE_COAP_IGNORED);
    assert(!ack && !exchange.acknowledged);
    response.message_id = 0x1235;
    assert(core_coap_exchange_receive(&exchange, 7, &response, 1, &ack) == CORE_COAP_IGNORED);
    response.message_id = 0x1234;
    assert(core_coap_exchange_receive(&exchange, 7, &response, 1, &ack) == CORE_COAP_ACKNOWLEDGED);
    assert(core_coap_exchange_poll(&exchange, UINT64_C(9000000)) == CORE_COAP_WAIT);
    assert(exchange.deadline_us == UINT64_C(10000000));
    response = (core_coap_message){.type = CORE_COAP_CON, .code = CORE_COAP_CONTENT,
                                   .message_id = 55, .token = {0xff}, .token_size = 1};
    assert(core_coap_exchange_receive(&exchange, 7, &response, 2, &ack) == CORE_COAP_IGNORED);
    assert(ack && exchange.active);
    response.token[0] = 0xa0;
    response.token[1] = 0x01;
    response.token_size = 2;
    assert(core_coap_exchange_receive(&exchange, 7, &response, 3, &ack) == CORE_COAP_RESPONSE);
    assert(ack && !exchange.active);
    assert(core_coap_exchange_receive(&exchange, 7, &response, 4, &ack) == CORE_COAP_IGNORED);
    assert(ack);

    assert(core_coap_exchange_start(&exchange, 7, bytes, size, 0, UINT64_C(3000000),
                                    CORE_COAP_ACK_TIMEOUT_US));
    assert(core_coap_exchange_poll(&exchange, UINT64_C(2000000)) == CORE_COAP_SEND);
    assert(core_coap_exchange_poll(&exchange, UINT64_C(3000000)) == CORE_COAP_TIMED_OUT);
    assert(core_coap_exchange_start(&exchange, 7, bytes, size, 0, UINT64_C(3000000),
                                    CORE_COAP_ACK_TIMEOUT_US));
    response = (core_coap_message){.type = CORE_COAP_RST, .message_id = 0x1234};
    assert(core_coap_exchange_receive(&exchange, 7, &response, 2, &ack) == CORE_COAP_RESET);
    assert(!exchange.active && !ack);
}

static void cached_responses(void)
{
    core_coap_cache cache = {0};
    core_coap_cached_response *entry;
    uint8_t response[] = {0x60, CORE_COAP_CHANGED, 0, 1};
    assert(core_coap_cache_reserve(&cache, 7, 1, 0, &entry) == CORE_COAP_CACHE_NEW);
    assert(core_coap_cache_store(entry, response, sizeof(response)));
    assert(!core_coap_cache_store(entry, response, sizeof(response)));
    assert(core_coap_cache_reserve(&cache, 7, 1, 1, &entry) == CORE_COAP_CACHE_DUPLICATE);
    assert(entry->response_size == sizeof(response) && memcmp(entry->response, response, sizeof(response)) == 0);
    assert(entry->expires_us == CORE_COAP_EXCHANGE_LIFETIME_US);
    for (uint16_t i = 2; i <= CORE_COAP_CACHE_SIZE; ++i) {
        assert(core_coap_cache_reserve(&cache, 7, i, 0, &entry) == CORE_COAP_CACHE_NEW);
    }
    assert(core_coap_cache_reserve(&cache, 7, 50, 1, &entry) == CORE_COAP_CACHE_FULL);
    assert(core_coap_cache_reserve(&cache, 7, 1, CORE_COAP_EXCHANGE_LIFETIME_US - 1,
                                   &entry) == CORE_COAP_CACHE_DUPLICATE);
    assert(core_coap_cache_reserve(&cache, 7, 50, CORE_COAP_EXCHANGE_LIFETIME_US,
                                   &entry) == CORE_COAP_CACHE_NEW);
}

static void block1(void)
{
    core_coap_block1 assembly = {0};
    const uint8_t tag[] = {1, 2};
    uint8_t body[35];
    for (size_t i = 0; i < sizeof(body); ++i) {
        body[i] = (uint8_t)i;
    }
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x18, sizeof(body), body + 16, 16) == CORE_COAP_BLOCK_INVALID);
    assert(!assembly.active);
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x08, sizeof(body), body, 16) == CORE_COAP_BLOCK_MORE);
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x08, sizeof(body), body, 16) == CORE_COAP_BLOCK_DUPLICATE);
    assert(assembly.size == 16);
    assert(core_coap_block1_receive(&assembly, 8, tag, 2, 0x18, 0, body + 16, 16) == CORE_COAP_BLOCK_INVALID);
    assert(core_coap_block1_receive(&assembly, 7, tag, 1, 0x18, 0, body + 16, 16) == CORE_COAP_BLOCK_INVALID);
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x20, 0, body + 32, 3) == CORE_COAP_BLOCK_INVALID);
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x18, 0, body + 16, 16) == CORE_COAP_BLOCK_MORE);
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x20, 0, body + 32, 2) == CORE_COAP_BLOCK_INVALID);
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x20, 0, body + 32, 3) == CORE_COAP_BLOCK_COMPLETE);
    assert(assembly.complete && assembly.size == sizeof(body) && memcmp(assembly.body, body, sizeof(body)) == 0);
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x20, 0, body + 32, 3) == CORE_COAP_BLOCK_DUPLICATE);
    body[32] ^= 1;
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x20, 0, body + 32, 3) == CORE_COAP_BLOCK_INVALID);
    core_coap_block1_reset(&assembly);
    assert(!assembly.active && !assembly.complete && assembly.size == 0);
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x07, 0, body, 16) == CORE_COAP_BLOCK_INVALID);

    uint8_t large[CORE_COAP_MAX_BODY];
    memset(large, 42, sizeof(large));
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x0e, sizeof(large), large, 1024) == CORE_COAP_BLOCK_MORE);
    assert(core_coap_block1_receive(&assembly, 7, tag, 2, 0x16, 0, large + 1024, 1024) == CORE_COAP_BLOCK_COMPLETE);
    assert(assembly.size == CORE_COAP_MAX_BODY);
}

static void bounded_decode_inputs(void)
{
    /* Deterministic malformed-input sweep: every accepted message must encode
     * back within the bounded buffer and preserve its message semantics. */
    uint32_t random = UINT32_C(0x39ad711b);
    uint8_t bytes[CORE_COAP_MAX_DATAGRAM], encoded[CORE_COAP_MAX_DATAGRAM];
    for (size_t trial = 0; trial < 10000; ++trial) {
        const size_t size = trial % sizeof(bytes);
        for (size_t i = 0; i < size; ++i) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[i] = (uint8_t)(random >> 24);
        }
        core_coap_message decoded, again;
        if (core_coap_decode(bytes, size, &decoded)) {
            size_t encoded_size;
            assert(core_coap_encode(&decoded, encoded, sizeof(encoded), &encoded_size));
            assert(core_coap_decode(encoded, encoded_size, &again));
            assert(again.type == decoded.type && again.code == decoded.code &&
                   again.message_id == decoded.message_id &&
                   again.payload_size == decoded.payload_size);
        }
    }
}

int main(void)
{
    codec();
    exchanges();
    cached_responses();
    block1();
    bounded_decode_inputs();
    puts("CoAP codec, exchange, bounded cache and Block1 tests passed");
    return 0;
}

#include "kaos_coap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void option_policy(void)
{
    const uint16_t supported[] = {CORE_COAP_URI_PATH, CORE_COAP_CONTENT_FORMAT};
    core_coap_message message = {.option_count = 2,
        .options = {{CORE_COAP_URI_PATH, NULL, 0}, {CORE_COAP_CONTENT_FORMAT, NULL, 0}}};
    CHECK(core_coap_options_supported(&message, supported, 2));
    message.options[message.option_count++] = (core_coap_option){100, NULL, 0};
    CHECK(core_coap_options_supported(&message, supported, 2)); /* elective */
    message.options[message.option_count++] = (core_coap_option){100, NULL, 0};
    CHECK(core_coap_options_supported(&message, supported, 2)); /* unknown repetition */
    message.options[2].number = 101;
    CHECK(!core_coap_options_supported(&message, supported, 2)); /* critical */
    message.option_count = 2;
    message.options[1].number = CORE_COAP_URI_PATH;
    CHECK(!core_coap_options_supported(&message, supported, 2)); /* singleton */
    message.option_count = 0;
    CHECK(core_coap_options_supported(&message, NULL, 0));
}

static void replies_and_replay(void)
{
    core_coap_message request = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = 42, .token_size = 2, .token = {0xab, 0xcd}};
    const uint8_t body[] = {0x81, 0};
    const uint8_t block_value = 0x10;
    const core_coap_option block = {CORE_COAP_BLOCK1, &block_value, 1};
    uint8_t encoded[CORE_COAP_MAX_DATAGRAM], replay[CORE_COAP_MAX_DATAGRAM];
    size_t size = 0, written = 0;
    CHECK(core_coap_reply(&request, CORE_COAP_CONTENT, 300, body, sizeof(body),
                          &block, encoded, sizeof(encoded), &size));
    core_coap_message decoded;
    CHECK(core_coap_decode(encoded, size, &decoded));
    CHECK(decoded.type == CORE_COAP_ACK && decoded.code == CORE_COAP_CONTENT);
    CHECK(decoded.message_id == 42 && decoded.token_size == 2);
    CHECK(memcmp(decoded.token, request.token, 2) == 0);
    CHECK(decoded.payload_size == sizeof(body) && memcmp(decoded.payload, body, sizeof(body)) == 0);
    uint32_t value;
    CHECK(core_coap_option_uint(core_coap_find(&decoded, CORE_COAP_CONTENT_FORMAT), &value));
    CHECK(value == 300);
    CHECK(core_coap_option_uint(core_coap_find(&decoded, CORE_COAP_BLOCK1), &value));
    CHECK(value == block_value);

    core_coap_cache cache = {0};
    core_coap_cached_response *entry;
    CHECK(core_coap_cache_reserve(&cache, 9, 42, 10, &entry) == CORE_COAP_CACHE_NEW);
    CHECK(core_coap_cache_store(entry, encoded, size));
    const core_coap_cache before = cache;
    CHECK(core_coap_cache_replay(&cache, 9, 42, 11, replay, sizeof(replay), &written));
    CHECK(written == size && memcmp(replay, encoded, size) == 0);
    memset(replay, 0x5a, sizeof(replay));
    written = 123;
    CHECK(!core_coap_cache_replay(&cache, 8, 42, 11, replay, sizeof(replay), &written));
    CHECK(!core_coap_cache_replay(&cache, 9, 41, 11, replay, sizeof(replay), &written));
    CHECK(!core_coap_cache_replay(&cache, 9, 42, 11, replay, size - 1, &written));
    CHECK(!core_coap_cache_replay(&cache, 9, 42, entry->expires_us,
                                 replay, sizeof(replay), &written));
    CHECK(written == 123);
    for (size_t i = 0; i < sizeof(replay); ++i) CHECK(replay[i] == 0x5a);
    CHECK(memcmp(&before, &cache, sizeof(cache)) == 0);

    CHECK(core_coap_reply(&request, CORE_COAP_BAD_REQUEST, 60, NULL, 0, NULL,
                          encoded, sizeof(encoded), &size));
    CHECK(core_coap_decode(encoded, size, &decoded));
    CHECK(decoded.option_count == 0 && decoded.payload_size == 0);
    CHECK(core_coap_reply(&request, CORE_COAP_CONTENT, 0, body, sizeof(body), NULL,
                          encoded, sizeof(encoded), &size));
    CHECK(core_coap_decode(encoded, size, &decoded));
    CHECK(core_coap_option_uint(core_coap_find(&decoded, CORE_COAP_CONTENT_FORMAT), &value));
    CHECK(value == 0);
    CHECK(core_coap_reset(request.message_id, encoded, sizeof(encoded), &size));
    CHECK(core_coap_decode(encoded, size, &decoded));
    CHECK(decoded.type == CORE_COAP_RST && decoded.code == 0 && decoded.message_id == 42);
    CHECK(decoded.token_size == 0 && decoded.option_count == 0 && decoded.payload_size == 0);
    written = 123;
    CHECK(!core_coap_reset(42, encoded, 3, &written) && written == 123);
}

int main(void)
{
    option_policy();
    replies_and_replay();
    puts("CoAP server helpers: option policy, reply encoding and read-only replay passed");
    return 0;
}

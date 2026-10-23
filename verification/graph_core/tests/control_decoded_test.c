#include "admission_fixture.h"
#include "kaos_control.h"

/* Exercise both public entrypoints at the same stable registry address. Restoring
 * the snapshot preserves every internal pointer before comparing effects. */
static core_admission pool, before_pool, after_pool;
static core_control control, before_control, after_control;
static size_t comparisons;

static void compare(const uint8_t *wire, size_t size, uint64_t sender,
                    uint64_t now, size_t capacity)
{
    core_coap_message request;
    CHECK(core_coap_decode(wire, size, &request));
    memcpy(&before_pool, &pool, sizeof(pool));
    memcpy(&before_control, &control, sizeof(control));
    uint8_t raw[CORE_COAP_MAX_DATAGRAM], decoded[CORE_COAP_MAX_DATAGRAM];
    memset(raw, 0xa5, sizeof(raw));
    memset(decoded, 0xa5, sizeof(decoded));
    size_t raw_size = SIZE_MAX, decoded_size = SIZE_MAX;
    const bool raw_result = core_control_receive(&control, sender, wire, size, now,
                                                 raw, capacity, &raw_size);
    memcpy(&after_pool, &pool, sizeof(pool));
    memcpy(&after_control, &control, sizeof(control));
    memcpy(&pool, &before_pool, sizeof(pool));
    memcpy(&control, &before_control, sizeof(control));
    const bool decoded_result = core_control_receive_message(&control, sender, &request,
        now, decoded, capacity, &decoded_size);
    CHECK(raw_result == decoded_result && raw_size == decoded_size);
    CHECK(memcmp(raw, decoded, sizeof(raw)) == 0);
    CHECK(memcmp(&pool, &after_pool, sizeof(pool)) == 0);
    CHECK(memcmp(&control, &after_control, sizeof(control)) == 0);
    if (capacity < CORE_COAP_MAX_DATAGRAM) {
        CHECK(!decoded_result && decoded_size == SIZE_MAX);
        CHECK(memcmp(&pool, &before_pool, sizeof(pool)) == 0);
        CHECK(memcmp(&control, &before_control, sizeof(control)) == 0);
    }
    ++comparisons;
}

static void check_message(const core_coap_message *message, uint64_t sender,
                          uint64_t now, size_t capacity)
{
    uint8_t wire[CORE_COAP_MAX_DATAGRAM];
    size_t size;
    CHECK(core_coap_encode(message, wire, sizeof(wire), &size));
    compare(wire, size, sender, now, capacity);
}

int main(void)
{
    fixture_pool(&pool, 1);
    control = (core_control){.admission = &pool};
    fixture_request node = fixture_node(40, 40);
    uint8_t body[CORE_MAX_REQUEST_BYTES];
    const size_t body_size = fixture_encode(&node, body);
    const uint8_t cbor = 60;
    core_coap_message request = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = 1, .token_size = 2, .token = {0xab, 0xcd},
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)"graph", 5},
                    {CORE_COAP_CONTENT_FORMAT, &cbor, 1}}, .option_count = 2,
        .payload = body, .payload_size = body_size};

    check_message(&request, 99, 10, CORE_COAP_MAX_DATAGRAM - 1);
    check_message(&request, 98, 10, CORE_COAP_MAX_DATAGRAM); /* unauthorized */
    check_message(&request, 99, 10, CORE_COAP_MAX_DATAGRAM); /* admission */
    const size_t reserved = pool.reserved_bytes;
    CHECK(reserved != 0);
    request.code = CORE_COAP_GET;
    check_message(&request, 99, 11, CORE_COAP_MAX_DATAGRAM); /* replay before method */
    for (uint16_t mid = 2; mid <= 6; ++mid) {
        request.message_id = mid;
        request.code = CORE_COAP_POST;
        check_message(&request, 99, 11, CORE_COAP_MAX_DATAGRAM); /* duplicate/cache full */
    }
    CHECK(pool.reserved_bytes == reserved);
    check_message(&request, 99, CORE_COAP_EXCHANGE_LIFETIME_US + 20,
                  CORE_COAP_MAX_DATAGRAM); /* expired cache entry */

    const char *paths[] = {"graph", "query", "prepare", "cancel", "activate", "owner", "stored", "unknown"};
    const uint8_t formats[] = {0, 42, 60};
    uint16_t mid = 20;
    for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); ++p) {
        for (size_t f = 0; f < sizeof(formats); ++f) {
            for (unsigned variant = 0; variant < 4; ++variant) {
                request.message_id = mid++;
                request.option_count = 2;
                request.options[0] = (core_coap_option){CORE_COAP_URI_PATH,
                    (const uint8_t *)paths[p], strlen(paths[p])};
                request.options[1] = (core_coap_option){CORE_COAP_CONTENT_FORMAT, &formats[f], 1};
                if (variant == 1) { /* repeated URI-Path must remain NOT_FOUND */
                    request.options[2] = request.options[1];
                    request.options[1] = request.options[0];
                    request.option_count = 3;
                } else if (variant >= 2) {
                    request.options[2] = (core_coap_option){variant == 2 ? 99 : 100, NULL, 0};
                    request.option_count = 3;
                }
                check_message(&request, 99, 30, CORE_COAP_MAX_DATAGRAM);
            }
        }
    }
    request = (core_coap_message){.type = CORE_COAP_CON, .message_id = mid++};
    check_message(&request, 99, 30, CORE_COAP_MAX_DATAGRAM); /* ping/reset */
    request.type = CORE_COAP_NON;
    request.code = CORE_COAP_GET;
    check_message(&request, 99, 30, CORE_COAP_MAX_DATAGRAM); /* no control response */

    /* Final Block1 replay must work after the assembly workspace is released. */
    fixture_pool(&pool, 1);
    control = (core_control){.admission = &pool};
    for (size_t offset = 0; offset < body_size; offset += 16) {
        const size_t size = body_size - offset < 16 ? body_size - offset : 16;
        const bool more = offset + size < body_size;
        const uint8_t tag = 1;
        uint8_t block[4], declared[4];
        const size_t bn = core_coap_uint((uint32_t)offset | (more ? 8u : 0u), block);
        const size_t sn = core_coap_uint((uint32_t)body_size, declared);
        request = (core_coap_message){.type = CORE_COAP_CON, .code = CORE_COAP_POST,
            .message_id = mid++, .payload = body + offset, .payload_size = size,
            .option_count = 5, .options = {
                {CORE_COAP_URI_PATH, (const uint8_t *)"graph", 5},
                {CORE_COAP_CONTENT_FORMAT, &cbor, 1}, {CORE_COAP_BLOCK1, block, bn},
                {CORE_COAP_SIZE1, declared, sn}, {CORE_COAP_REQUEST_TAG, &tag, 1}}};
        check_message(&request, 99, 40, CORE_COAP_MAX_DATAGRAM - 1);
        check_message(&request, 99, 40, CORE_COAP_MAX_DATAGRAM);
        check_message(&request, 99, 41, CORE_COAP_MAX_DATAGRAM);
    }
    CHECK(!control.assembly.active && pool.reserved_bytes == reserved);
    printf("decoded control equivalence: %zu comparisons\n", comparisons);
    return 0;
}

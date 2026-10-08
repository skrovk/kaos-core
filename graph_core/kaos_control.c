#include "kaos_control.h"
#include "kaos_cbor.h"

#include <string.h>

static bool path_is(const core_coap_message *m, const char *path)
{
    const core_coap_option *option = core_coap_find(m, CORE_COAP_URI_PATH);
    return option != NULL && option->size == strlen(path) &&
        memcmp(option->value, path, option->size) == 0;
}

static bool reply(const core_coap_message *request, uint8_t code,
                  const uint8_t *body, size_t size, const core_coap_option *block,
                  uint8_t *out, size_t capacity, size_t *written)
{
    const uint8_t format = 60;
    core_coap_message response = {.type = CORE_COAP_ACK, .code = code,
        .message_id = request->message_id, .token_size = request->token_size,
        .payload = body, .payload_size = size};
    memcpy(response.token, request->token, request->token_size);
    if (size) response.options[response.option_count++] =
        (core_coap_option){CORE_COAP_CONTENT_FORMAT, &format, 1};
    if (block != NULL) response.options[response.option_count++] = *block;
    return core_coap_encode(&response, out, capacity, written);
}

static bool query_id(const uint8_t *data, size_t size, core_op_id *id)
{
    core_cbor_reader reader = {.data = data, .size = size};
    uint64_t n, version, kind;
    const uint8_t *body;
    size_t length;
    return core_cbor_array(&reader, &n) && n == 4 &&
        core_cbor_uint(&reader, &version) && version == 1 &&
        core_cbor_uint(&reader, &kind) && kind == CORE_QUERY_KIND &&
        core_cbor_uint(&reader, &id->value) &&
        core_cbor_bytes(&reader, &body, &length) && length == 0 && reader.offset == size;
}

bool core_control_receive(core_control *c, uint64_t sender,
                           const uint8_t *data, size_t size, uint64_t now,
                           uint8_t *out, size_t capacity, size_t *written)
{
    if (capacity < CORE_COAP_MAX_DATAGRAM) return false;
    core_coap_message m;
    if (!core_coap_decode(data, size, &m) || m.type != CORE_COAP_CON) return false;
    if (m.code == 0) {
        const core_coap_message reset = {.type = CORE_COAP_RST, .message_id = m.message_id};
        return core_coap_encode(&reset, out, capacity, written);
    }
    if (m.code >= 32) return false;
    /* Replay precedes body reassembly: a final Block1 retry remains valid
     * after its workspace has been released for the next transfer. */
    for (size_t i = 0; i < CORE_COAP_CACHE_SIZE; ++i) {
        const core_coap_cached_response *cached = &c->cache.entries[i];
        if (cached->occupied && cached->peer == sender &&
            cached->message_id == m.message_id && now < cached->expires_us) {
            memcpy(out, cached->response, cached->response_size);
            *written = cached->response_size;
            return true;
        }
    }
    if (m.code != CORE_COAP_POST)
        return reply(&m, 133, NULL, 0, NULL, out, capacity, written); /* 4.05 */
    uint8_t error = 0;
    bool seen_path = false, seen_format = false, seen_block = false;
    bool seen_size = false, seen_tag = false;
    for (size_t i = 0; i < m.option_count; ++i) {
        const uint16_t n = m.options[i].number;
        bool *seen = n == CORE_COAP_URI_PATH ? &seen_path :
            n == CORE_COAP_CONTENT_FORMAT ? &seen_format :
            n == CORE_COAP_BLOCK1 ? &seen_block :
            n == CORE_COAP_SIZE1 ? &seen_size :
            n == CORE_COAP_REQUEST_TAG ? &seen_tag : NULL;
        if (seen != NULL) {
            if (*seen) error = CORE_COAP_BAD_OPTION;
            *seen = true;
        } else if (n & 1u) error = CORE_COAP_BAD_OPTION;
    }
    uint32_t format;
    if (!core_coap_option_uint(core_coap_find(&m, CORE_COAP_CONTENT_FORMAT), &format) || format != 60)
        error = CORE_COAP_BAD_REQUEST;
    const bool graph = path_is(&m, "graph"), prepare = path_is(&m, "prepare");
    const bool cancel = path_is(&m, "cancel");
    const bool query = path_is(&m, "query");
    if (!graph && !prepare && !cancel && !query) error = CORE_COAP_NOT_FOUND;
    if (error) return reply(&m, error, NULL, 0, NULL, out, capacity, written);
    if (!c->admission->configured || (sender != c->admission->authority && !prepare && !cancel))
        return reply(&m, CORE_COAP_UNAUTHORIZED, NULL, 0, NULL, out, capacity, written);
    if (prepare || cancel) {
        bool enrolled = false;
        for (size_t i = 0; i < c->admission->peer_count; ++i)
            enrolled |= c->admission->peers[i] == sender;
        if (!enrolled)
            return reply(&m, CORE_COAP_UNAUTHORIZED, NULL, 0, NULL, out, capacity, written);
    }

    const uint8_t *body = m.payload;
    size_t body_size = m.payload_size;
    const core_coap_option *block = core_coap_find(&m, CORE_COAP_BLOCK1);
    if (block != NULL) {
        const core_coap_option *tag = core_coap_find(&m, CORE_COAP_REQUEST_TAG);
        uint32_t value, declared;
        /* Supported cancellation bodies fit one datagram. Unsegmented cleanup
         * can always recompute a lost reply without ordinary cache capacity. */
        if (query || cancel || tag == NULL || !core_coap_option_uint(block, &value) ||
            !core_coap_option_uint(core_coap_find(&m, CORE_COAP_SIZE1), &declared) ||
            declared == 0 || declared > CORE_MAX_REQUEST_BYTES)
            return reply(&m, CORE_COAP_BAD_REQUEST, NULL, 0, NULL, out, capacity, written);
        if (c->assembly.active && !core_before_deadline(now, c->assembly_deadline_us))
            core_coap_block1_reset(&c->assembly);
        const uint8_t resource = graph ? 1 : prepare ? 2 : 3;
        if (c->assembly.active && c->assembly_resource != resource)
            return reply(&m, CORE_COAP_INCOMPLETE, NULL, 0, NULL, out, capacity, written);
        if (!c->assembly.active) {
            if (!core_deadline(now, CORE_COAP_EXCHANGE_LIFETIME_US, &c->assembly_deadline_us))
                return reply(&m, CORE_COAP_UNAVAILABLE, NULL, 0, NULL, out, capacity, written);
            c->assembly_resource = resource;
        }
        const core_coap_block_result result = core_coap_block1_receive(&c->assembly,
            sender, tag->value, tag->size, value, declared, body, body_size);
        if (result == CORE_COAP_BLOCK_INVALID)
            return reply(&m, CORE_COAP_INCOMPLETE, NULL, 0, NULL, out, capacity, written);
        if (!c->assembly.complete)
            return reply(&m, CORE_COAP_CONTINUE, NULL, 0, block, out, capacity, written);
        body = c->assembly.body;
        body_size = c->assembly.size;
    }
    if (body_size == 0 || body_size > CORE_MAX_REQUEST_BYTES)
        return reply(&m, CORE_COAP_TOO_LARGE, NULL, 0, NULL, out, capacity, written);
    if (query) {
        core_op_id id;
        if (!query_id(body, body_size, &id))
            return reply(&m, CORE_COAP_BAD_REQUEST, NULL, 0, NULL, out, capacity, written);
        const core_admission_record *record = core_admission_find(c->admission, id);
        uint8_t encoded[CORE_REPORT_BYTES];
        size_t encoded_size;
        if (record == NULL || record->endpoint_only || !core_report_encode(&record->evidence, encoded,
                                                 sizeof(encoded), &encoded_size))
            return reply(&m, CORE_COAP_NOT_FOUND, NULL, 0, NULL, out, capacity, written);
        return reply(&m, CORE_COAP_CONTENT, encoded, encoded_size, NULL, out, capacity, written);
    }
    core_coap_cached_response *cached = NULL;
    /* Owner cleanup is idempotent and needs service even at transport-cache
     * saturation. RFC 7252 permits recomputing idempotent responses. */
    const core_coap_cache_result cache = cancel ? CORE_COAP_CACHE_NEW :
        core_coap_cache_reserve(&c->cache, sender, m.message_id, now, &cached);
    if (cache == CORE_COAP_CACHE_FULL)
        return reply(&m, CORE_COAP_UNAVAILABLE, NULL, 0, NULL, out, capacity, written);
    if (cache == CORE_COAP_CACHE_DUPLICATE) {
        memcpy(out, cached->response, cached->response_size);
        *written = cached->response_size;
        return true;
    }
    const core_admission_record *record;
    const core_admit_result result = cancel ?
        core_endpoint_cancel(c->admission, sender, body, body_size, now, &record) : prepare ?
        core_endpoint_submit(c->admission, sender, body, body_size, now, &record) :
        core_request_submit(c->admission, sender, body, body_size, now, &record);
    const uint8_t code = result == CORE_ADMIT_ACCEPTED ? CORE_COAP_CREATED :
        result == CORE_ADMIT_DUPLICATE ? CORE_COAP_CHANGED :
        result == CORE_ADMIT_FULL ? CORE_COAP_UNAVAILABLE :
        result == CORE_ADMIT_UNAUTHORIZED ? CORE_COAP_UNAUTHORIZED : CORE_COAP_BAD_REQUEST;
    if (!reply(&m, code, NULL, 0, block, out, capacity, written)) return false;
    const bool stored = cached == NULL || core_coap_cache_store(cached, out, *written);
    if (block != NULL) core_coap_block1_reset(&c->assembly);
    return stored;
}

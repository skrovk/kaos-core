#include "kaos_control.h"
#include "kaos_cbor.h"

#include <string.h>

typedef enum {
    CONTROL_NOT_FOUND, CONTROL_GRAPH, CONTROL_PREPARE, CONTROL_CANCEL, CONTROL_QUERY
} control_resource;

static bool path_is(const core_coap_message *message, const char *path)
{
    const core_coap_option *option = core_coap_find(message, CORE_COAP_URI_PATH);
    return option != NULL && option->size == strlen(path) &&
        memcmp(option->value, path, option->size) == 0;
}

static control_resource route(const core_coap_message *message)
{
    if (path_is(message, "graph")) return CONTROL_GRAPH;
    if (path_is(message, "prepare")) return CONTROL_PREPARE;
    if (path_is(message, "cancel")) return CONTROL_CANCEL;
    if (path_is(message, "query")) return CONTROL_QUERY;
    return CONTROL_NOT_FOUND;
}

static uint8_t request_error(const core_coap_message *message, control_resource resource)
{
    static const uint16_t options[] = {CORE_COAP_URI_PATH, CORE_COAP_CONTENT_FORMAT,
        CORE_COAP_BLOCK1, CORE_COAP_SIZE1, CORE_COAP_REQUEST_TAG};
    uint8_t error = core_coap_options_supported(message, options,
        sizeof(options) / sizeof(options[0])) ? 0 : CORE_COAP_BAD_OPTION;
    uint32_t format;
    if (!core_coap_option_uint(core_coap_find(message, CORE_COAP_CONTENT_FORMAT), &format) ||
        format != 60) error = CORE_COAP_BAD_REQUEST;
    /* Preserve the resource/format/option error precedence of this interface. */
    if (resource == CONTROL_NOT_FOUND) error = CORE_COAP_NOT_FOUND;
    return error;
}

static bool authorized(const core_admission *pool, uint64_t sender, control_resource resource)
{
    if (!pool->configured) return false;
    if (resource != CONTROL_PREPARE && resource != CONTROL_CANCEL)
        return sender == pool->authority;
    for (size_t i = 0; i < pool->peer_count; ++i)
        if (pool->peers[i] == sender) return true;
    return false;
}

/* CBOR is the graph control representation; CoAP owns response framing. */
static bool reply(const core_coap_message *request, uint8_t code,
                  const uint8_t *body, size_t size, const core_coap_option *block,
                  uint8_t *out, size_t capacity, size_t *written)
{
    return core_coap_reply(request, code, 60, body, size, block, out, capacity, written);
}

/* Zero means a complete body is available; otherwise return a CoAP response
 * code. Only graph/prepare use multipart bodies. Authorization precedes this
 * workspace mutation, and its fixed deadline never extends on retries. */
static uint8_t receive_body(core_control *control, uint64_t sender,
                            control_resource resource, const core_coap_message *request,
                            uint64_t now, const uint8_t **body, size_t *size)
{
    *body = request->payload;
    *size = request->payload_size;
    const core_coap_option *block = core_coap_find(request, CORE_COAP_BLOCK1);
    if (block != NULL) {
        const core_coap_option *tag = core_coap_find(request, CORE_COAP_REQUEST_TAG);
        uint32_t value, declared;
        if (resource == CONTROL_QUERY || resource == CONTROL_CANCEL || tag == NULL ||
            !core_coap_option_uint(block, &value) ||
            !core_coap_option_uint(core_coap_find(request, CORE_COAP_SIZE1), &declared) ||
            declared == 0 || declared > CORE_MAX_REQUEST_BYTES)
            return CORE_COAP_BAD_REQUEST;
        if (control->assembly.active && !core_before_deadline(now, control->assembly_deadline_us))
            core_coap_block1_reset(&control->assembly);
        if (control->assembly.active && control->assembly_resource != resource)
            return CORE_COAP_INCOMPLETE;
        if (!control->assembly.active) {
            if (!core_deadline(now, CORE_COAP_EXCHANGE_LIFETIME_US, &control->assembly_deadline_us))
                return CORE_COAP_UNAVAILABLE;
            control->assembly_resource = (uint8_t)resource;
        }
        const core_coap_block_result result = core_coap_block1_receive(&control->assembly,
            sender, tag->value, tag->size, value, declared, *body, *size);
        if (result == CORE_COAP_BLOCK_INVALID) return CORE_COAP_INCOMPLETE;
        if (!control->assembly.complete) return CORE_COAP_CONTINUE;
        *body = control->assembly.body;
        *size = control->assembly.size;
    }
    return *size == 0 || *size > CORE_MAX_REQUEST_BYTES ? CORE_COAP_TOO_LARGE : 0;
}

static bool query_id(const uint8_t *data, size_t size, core_op_id *id)
{
    core_cbor_reader reader = {.data = data, .size = size};
    uint64_t n, version, kind;
    const uint8_t *body;
    size_t length;
    return core_cbor_array(&reader, &n) && n == 4 &&
        core_cbor_uint(&reader, &version) && version == CORE_SCHEMA_VERSION &&
        core_cbor_uint(&reader, &kind) && kind == CORE_QUERY_KIND &&
        core_cbor_uint(&reader, &id->value) &&
        core_cbor_bytes(&reader, &body, &length) && length == 0 && reader.offset == size;
}

static bool query_reply(const core_admission *pool, const core_coap_message *request,
                        const uint8_t *body, size_t size,
                        uint8_t *out, size_t capacity, size_t *written)
{
    core_op_id id;
    if (!query_id(body, size, &id))
        return reply(request, CORE_COAP_BAD_REQUEST, NULL, 0, NULL, out, capacity, written);
    const core_admission_record *record = core_admission_find(pool, id);
    uint8_t encoded[CORE_REPORT_BYTES];
    size_t encoded_size;
    if (record == NULL || record->endpoint_only ||
        !core_report_encode(&record->evidence, encoded, sizeof(encoded), &encoded_size))
        return reply(request, CORE_COAP_NOT_FOUND, NULL, 0, NULL, out, capacity, written);
    return reply(request, CORE_COAP_CONTENT, encoded, encoded_size, NULL, out, capacity, written);
}

static uint8_t submit(core_admission *pool, uint64_t sender, control_resource resource,
                       const uint8_t *body, size_t size, uint64_t now)
{
    const core_admission_record *record;
    core_admit_result result;
    switch (resource) {
    case CONTROL_GRAPH:
        result = core_request_submit(pool, sender, body, size, now, &record);
        break;
    case CONTROL_PREPARE:
        result = core_endpoint_submit(pool, sender, body, size, now, &record);
        break;
    case CONTROL_CANCEL:
        result = core_endpoint_cancel(pool, sender, body, size, now, &record);
        break;
    default:
        return CORE_COAP_NOT_FOUND;
    }
    switch (result) {
    case CORE_ADMIT_ACCEPTED: return CORE_COAP_CREATED;
    case CORE_ADMIT_DUPLICATE: return CORE_COAP_CHANGED;
    case CORE_ADMIT_FULL: return CORE_COAP_UNAVAILABLE;
    case CORE_ADMIT_UNAUTHORIZED: return CORE_COAP_UNAUTHORIZED;
    default: return CORE_COAP_BAD_REQUEST;
    }
}

bool core_control_receive(core_control *control, uint64_t sender,
                           const uint8_t *data, size_t size, uint64_t now,
                           uint8_t *out, size_t capacity, size_t *written)
{
    /* A response must fit before any admission or assembly mutation. */
    if (capacity < CORE_COAP_MAX_DATAGRAM) return false;
    core_coap_message request;
    if (!core_coap_decode(data, size, &request) || request.type != CORE_COAP_CON) return false;
    if (request.code == 0) return core_coap_reset(request.message_id, out, capacity, written);
    if (request.code >= 32) return false;

    /* A final Block1 retry must replay even after its workspace was released. */
    if (core_coap_cache_replay(&control->cache, sender, request.message_id, now,
                               out, capacity, written)) return true;
    if (request.code != CORE_COAP_POST)
        return reply(&request, CORE_COAP_METHOD_NOT_ALLOWED, NULL, 0, NULL, out, capacity, written);
    const control_resource resource = route(&request);
    const uint8_t error = request_error(&request, resource);
    if (error) return reply(&request, error, NULL, 0, NULL, out, capacity, written);
    if (!authorized(control->admission, sender, resource))
        return reply(&request, CORE_COAP_UNAUTHORIZED, NULL, 0, NULL, out, capacity, written);

    const uint8_t *body;
    size_t body_size;
    const core_coap_option *block = core_coap_find(&request, CORE_COAP_BLOCK1);
    const uint8_t body_result = receive_body(control, sender, resource, &request, now,
                                             &body, &body_size);
    if (body_result != 0)
        return reply(&request, body_result, NULL, 0,
                      body_result == CORE_COAP_CONTINUE ? block : NULL, out, capacity, written);
    if (resource == CONTROL_QUERY)
        return query_reply(control->admission, &request, body, body_size, out, capacity, written);

    /* Reserve reply capacity before admission. Idempotent cancellation must
     * still run when the ordinary reply cache is saturated. */
    core_coap_cached_response *cached = NULL;
    if (resource != CONTROL_CANCEL) {
        const core_coap_cache_result cache = core_coap_cache_reserve(&control->cache,
            sender, request.message_id, now, &cached);
        if (cache == CORE_COAP_CACHE_FULL)
            return reply(&request, CORE_COAP_UNAVAILABLE, NULL, 0, NULL, out, capacity, written);
        if (cache == CORE_COAP_CACHE_DUPLICATE)
            return core_coap_cache_replay(&control->cache, sender, request.message_id, now,
                                          out, capacity, written);
    }
    const uint8_t code = submit(control->admission, sender, resource, body, body_size, now);
    if (!reply(&request, code, NULL, 0, block, out, capacity, written)) return false;
    const bool stored = cached == NULL || core_coap_cache_store(cached, out, *written);
    if (block != NULL) core_coap_block1_reset(&control->assembly);
    return stored;
}

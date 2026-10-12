#include "kaos_control.h"
#include "kaos_cbor.h"

#include <string.h>

typedef enum {
    CONTROL_NOT_FOUND, CONTROL_GRAPH, CONTROL_PREPARE, CONTROL_CANCEL, CONTROL_QUERY,
    CONTROL_ACTIVATE, CONTROL_OWNER, CONTROL_STORED
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
    if (path_is(message, "activate")) return CONTROL_ACTIVATE;
    if (path_is(message, "owner")) return CONTROL_OWNER;
    if (path_is(message, "stored")) return CONTROL_STORED;
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

static bool authorized(const core_control *control, uint64_t sender, control_resource resource)
{
    const core_admission *pool = control->admission;
    if (!pool->configured) return false;
    if ((resource == CONTROL_QUERY || resource == CONTROL_STORED) &&
        control->collector_enabled && sender == control->collector) return true;
    if (resource != CONTROL_PREPARE && resource != CONTROL_CANCEL &&
        resource != CONTROL_ACTIVATE && resource != CONTROL_OWNER)
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
        if ((resource != CONTROL_GRAPH && resource != CONTROL_PREPARE) || tag == NULL ||
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

/* The complete request decoder still owns validation and admission. This
 * envelope view only identifies an idempotent edge cleanup resource and its
 * retained record after submit succeeds. */
static bool request_identity(const uint8_t *data, size_t size, uint64_t *kind, core_op_id *id)
{
    core_cbor_reader reader = {.data = data, .size = size};
    uint64_t count, version;
    const uint8_t *body;
    size_t body_size;
    return core_cbor_array(&reader, &count) && count == 4 &&
        core_cbor_uint(&reader, &version) && version == CORE_SCHEMA_VERSION &&
        core_cbor_uint(&reader, kind) && core_cbor_uint(&reader, &id->value) &&
        core_cbor_bytes(&reader, &body, &body_size) && reader.offset == size;
}

static size_t cleanup_snapshot(core_control *control, uint64_t sender,
                                 core_op_id id, uint64_t now, uint8_t *encoded)
{
    const core_admission_record *record = core_admission_find(control->admission, id);
    if (control->edges == NULL || !core_edge_supported(record) || !record->endpoint_only ||
        !record->cleanup_selected || record->scope.coordinator != sender ||
        record->scope.coordinator == control->admission->local_device) return 0;
    core_edge_step(control->edges, now);
    /* For a standalone edge, its source is Kc; the remote owner has exactly
     * the receiver role. Reuse the same cumulative stream and attribution. */
    const core_owner_report report = {.operation = id, .owner = control->admission->local_device,
        .object = 1, .facts = record->local_facts[1]};
    size_t size = 0;
    (void)core_owner_report_encode(&report, encoded, CORE_REPORT_BYTES, &size);
    return size;
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
    if (!core_admission_report_retained(record)) {
        core_cbor_writer w = {.data = encoded, .size = sizeof(encoded)};
        core_cbor_write_array(&w, 5);
        core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
        core_cbor_write_uint(&w, CORE_NO_RESULT_KIND);
        core_cbor_write_uint(&w, id.value);
        core_cbor_write_uint(&w, pool->local_device);
        const bool current = record != NULL && !record->endpoint_only && record->scope.kind != 0;
        core_cbor_write_array(&w, current ? 3 : 0);
        if (current) {
            core_cbor_write_uint(&w, record->evidence.revision);
            core_cbor_write_uint(&w, record->evidence.history);
            core_cbor_write_array(&w, record->evidence.object_count);
            for (uint8_t i = 0; i < record->evidence.object_count; ++i)
                core_cbor_write_uint(&w, record->evidence.objects[i]);
        }
        return reply(request, CORE_COAP_NOT_FOUND, encoded, w.offset, NULL, out, capacity, written);
    }
    if (!core_report_encode(&record->evidence, encoded, sizeof(encoded), &encoded_size))
        return reply(request, CORE_COAP_BAD_REQUEST, NULL, 0, NULL, out, capacity, written);
    return reply(request, CORE_COAP_CONTENT, encoded, encoded_size, NULL, out, capacity, written);
}

static bool storage_ack(core_admission *pool, const uint8_t *body, size_t size)
{
    core_cbor_reader rd = {.data = body, .size = size};
    uint64_t count, version, kind, coordinator, revision;
    core_op_id id;
    if (!core_cbor_array(&rd, &count) || count != 5 ||
        !core_cbor_uint(&rd, &version) || version != CORE_SCHEMA_VERSION ||
        !core_cbor_uint(&rd, &kind) || kind != CORE_STORAGE_ACK_KIND ||
        !core_cbor_uint(&rd, &id.value) || !core_cbor_uint(&rd, &coordinator) ||
        !core_cbor_uint(&rd, &revision) || revision > UINT8_MAX || rd.offset != size)
        return false;
    core_admission_acknowledge(pool, id, coordinator, (uint8_t)revision);
    return true;
}

static bool owner_report(core_control *control, uint64_t sender,
                          const core_coap_message *request, uint64_t now)
{
    core_owner_report report;
    if (!core_owner_report_decode(request->payload, request->payload_size, &report) ||
        report.owner != sender) return false;
    core_owner_receipt *receipt = NULL;
    for (size_t i = 0; i < control->admission->peer_count; ++i)
        if (control->admission->peers[i] == sender) receipt = &control->owner_receipts[i];
    if (receipt == NULL) return false;
    const bool retained = receipt->occupied && core_before_deadline(now, receipt->deadline_us);
    if (retained && receipt->message_id == request->message_id)
        return receipt->token_size == request->token_size &&
            memcmp(receipt->token, request->token, request->token_size) == 0 &&
            receipt->operation.value == report.operation.value && receipt->object == report.object &&
            receipt->facts == report.facts;
    if (!core_edge_owner(control->edges, sender, request->payload, request->payload_size, now))
        return false;
    /* Enrolled ports issue strictly increasing MIDs and refuse wrap. NSTART=1
     * lets the newest final exchange replace its predecessor, but a delayed
     * duplicate of that predecessor must never overwrite the newer receipt. */
    if ((report.facts & CORE_OBJECT_CLEANED) &&
        (!retained || request->message_id > receipt->message_id)) {
        *receipt = (core_owner_receipt){.operation = report.operation,
            .message_id = request->message_id, .token_size = request->token_size,
            .object = report.object, .facts = report.facts, .occupied = true};
        memcpy(receipt->token, request->token, request->token_size);
        if (!core_deadline(now, CORE_COAP_EXCHANGE_LIFETIME_US, &receipt->deadline_us))
            receipt->deadline_us = UINT64_MAX;
    }
    return true;
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
    if (!authorized(control, sender, resource))
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
    if (resource == CONTROL_STORED)
        return reply(&request, storage_ack(control->admission, body, body_size) ?
            CORE_COAP_CHANGED : CORE_COAP_BAD_REQUEST, NULL, 0, NULL, out, capacity, written);
    if (resource == CONTROL_ACTIVATE || resource == CONTROL_OWNER) {
        if (control->edges == NULL)
            return reply(&request, CORE_COAP_NOT_FOUND, NULL, 0, NULL, out, capacity, written);
        const bool accepted = resource == CONTROL_ACTIVATE ?
            core_edge_activate(control->edges, sender, body, body_size) :
            owner_report(control, sender, &request, now);
        return reply(&request, accepted ? CORE_COAP_CHANGED : CORE_COAP_BAD_REQUEST,
                      NULL, 0, NULL, out, capacity, written);
    }

    /* Forward one indexed channel, retaining creation and observer identity.
     * The admission record provides idempotency and exact descriptor checks;
     * no generic response-cache slot is needed for these bounded commands. */
    if (control->edges != NULL &&
        (resource == CONTROL_PREPARE || resource == CONTROL_CANCEL)) {
        core_cbor_reader rd = {.data = body, .size = body_size};
        uint64_t count, version, tag, index;
        core_op_id operation;
        if (core_cbor_array(&rd, &count) &&
            core_cbor_uint(&rd, &version) && version == CORE_SCHEMA_VERSION &&
            core_cbor_uint(&rd, &tag) &&
            ((count == 5 && tag == CORE_CHANNEL_COMMAND_KIND) ||
             (count == 6 && tag == CORE_REMOVAL_COMMAND_KIND && resource == CONTROL_PREPARE)) &&
            core_cbor_uint(&rd, &operation.value) && core_cbor_uint(&rd, &index)) {
            const core_admission_record *record = NULL;
            const bool removal = tag == CORE_REMOVAL_COMMAND_KIND;
            const core_admit_result result = removal ?
                core_endpoint_removal_submit(control->admission, sender, body, body_size, &record) :
                core_endpoint_channel_submit(control->admission, sender, body, body_size, now,
                    resource == CONTROL_CANCEL, &record);
            const bool accepted = result == CORE_ADMIT_ACCEPTED || result == CORE_ADMIT_DUPLICATE;
            uint8_t snapshot[CORE_REPORT_BYTES];
            size_t length = 0;
            if (accepted && (removal || resource == CONTROL_CANCEL) && record != NULL) {
                core_edge_step(control->edges, now);
                const core_channel *channel = &record->scope.bindings[index].channel;
                const uint8_t object = (uint8_t)(1 + 2 * index +
                    (channel->destination_owner == control->admission->local_device));
                const core_owner_report report = {.operation = operation,
                    .owner = control->admission->local_device, .object = object,
                    .facts = record->local_facts[object]};
                (void)core_owner_report_encode(&report, snapshot, sizeof(snapshot), &length);
            }
            const bool replied = reply(&request, accepted ? CORE_COAP_CHANGED :
                (result == CORE_ADMIT_FULL ? CORE_COAP_UNAVAILABLE : CORE_COAP_BAD_REQUEST),
                length ? snapshot : NULL, length, block, out, capacity, written);
            if (block != NULL) core_coap_block1_reset(&control->assembly);
            return replied;
        }
    }

    uint64_t kind = 0;
    core_op_id id = {0};
    const bool edge_cleanup = control->edges != NULL &&
        (resource == CONTROL_CANCEL || resource == CONTROL_PREPARE) &&
        request_identity(body, body_size, &kind, &id) &&
        ((resource == CONTROL_CANCEL && kind == CORE_REQUEST_ADD_EDGE) ||
         (resource == CONTROL_PREPARE && kind == CORE_REQUEST_REMOVE_EDGE));
    /* The bounded edge engine sends cleanup in one datagram. Its duplicate
     * replies must reflect later closure, even if an earlier response held
     * only CLEANUP. Multipart admission retains ordinary Block1 replay. */
    const bool fresh_cleanup_reply = edge_cleanup && block == NULL;
    /* Reserve reply capacity before admission. Idempotent cleanup must still
     * run when the ordinary reply cache is saturated. */
    core_coap_cached_response *cached = NULL;
    if (resource != CONTROL_CANCEL && !fresh_cleanup_reply) {
        const core_coap_cache_result cache = core_coap_cache_reserve(&control->cache,
            sender, request.message_id, now, &cached);
        if (cache == CORE_COAP_CACHE_FULL)
            return reply(&request, CORE_COAP_UNAVAILABLE, NULL, 0, NULL, out, capacity, written);
        if (cache == CORE_COAP_CACHE_DUPLICATE)
            return core_coap_cache_replay(&control->cache, sender, request.message_id, now,
                                          out, capacity, written);
    }
    const uint8_t code = submit(control->admission, sender, resource, body, body_size, now);
    uint8_t snapshot[CORE_REPORT_BYTES];
    const size_t snapshot_size = edge_cleanup &&
        (code == CORE_COAP_CREATED || code == CORE_COAP_CHANGED) ?
        cleanup_snapshot(control, sender, id, now, snapshot) : 0;
    if (!reply(&request, code, snapshot_size ? snapshot : NULL, snapshot_size,
                 block, out, capacity, written)) return false;
    const bool stored = cached == NULL || core_coap_cache_store(cached, out, *written);
    if (block != NULL) core_coap_block1_reset(&control->assembly);
    return stored;
}

#include "kaos_admission_internal.h"
#include "kaos_cbor.h"

#include <string.h>

static bool read_u16(core_cbor_reader *reader, uint16_t *out)
{
    uint64_t value;
    if (!core_cbor_uint(reader, &value) || value > UINT16_MAX) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool read_u32(core_cbor_reader *reader, uint32_t *out)
{
    uint64_t value;
    if (!core_cbor_uint(reader, &value) || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool read_ports(core_cbor_reader *reader, core_request_scope *scope)
{
    uint64_t count;
    if (!core_cbor_array(reader, &count) || count > CORE_MAX_PORTS) {
        return false;
    }
    scope->port_count = (uint8_t)count;
    for (size_t i = 0; i < scope->port_count; ++i) {
        core_port *port = &scope->ports[i];
        uint64_t fields, direction;
        if (!core_cbor_array(reader, &fields) || fields != 4 ||
            !read_u16(reader, &port->id) || !core_cbor_uint(reader, &direction) || direction > 1 ||
            !read_u16(reader, &port->type) || !read_u16(reader, &port->max_payload) ||
            port->max_payload == 0 || port->max_payload > CORE_MAX_PAYLOAD_BYTES) {
            return false;
        }
        port->direction = (uint8_t)direction;
        for (size_t j = 0; j < i; ++j) {
            if (scope->ports[j].id == port->id) {
                return false;
            }
        }
    }
    return true;
}

static bool read_channel(core_cbor_reader *reader, core_channel *channel)
{
    uint64_t count, queue_messages;
    if (!core_cbor_array(reader, &count) || count != 13 ||
        !core_cbor_uint(reader, &channel->source_logical) ||
        !core_cbor_uint(reader, &channel->destination_logical) ||
        channel->source_logical == channel->destination_logical ||
        !core_cbor_uint(reader, &channel->service) ||
        !core_cbor_uint(reader, &channel->source_instance.value) ||
        !core_cbor_uint(reader, &channel->destination_instance.value) ||
        channel->source_instance.value == channel->destination_instance.value ||
        !core_cbor_uint(reader, &channel->source_owner) ||
        !core_cbor_uint(reader, &channel->destination_owner) ||
        !read_u16(reader, &channel->source_port) ||
        !read_u16(reader, &channel->destination_port) ||
        !read_u16(reader, &channel->type) ||
        !read_u16(reader, &channel->payload_bytes) || channel->payload_bytes == 0 ||
        channel->payload_bytes > CORE_MAX_PAYLOAD_BYTES ||
        !core_cbor_uint(reader, &queue_messages) || queue_messages == 0 ||
        queue_messages > CORE_MAX_QUEUE_MESSAGES ||
        !core_cbor_uint(reader, &channel->addition_allowance_us) ||
        channel->addition_allowance_us == 0) {
        return false;
    }
    channel->queue_messages = (uint8_t)queue_messages;
    return true;
}

static bool read_binding(core_cbor_reader *reader, core_binding *binding)
{
    uint64_t count;
    return core_cbor_array(reader, &count) && count == 2 &&
           read_channel(reader, &binding->channel) &&
           core_cbor_uint(reader, &binding->creating_op.value);
}

static bool read_channels(core_cbor_reader *reader, core_request_scope *scope,
                          core_op_id id, bool bindings)
{
    uint64_t count;
    if (!core_cbor_array(reader, &count) || count > CORE_MAX_REQUEST_CHANNELS) {
        return false;
    }
    scope->channel_count = (uint8_t)count;
    for (size_t i = 0; i < scope->channel_count; ++i) {
        core_binding *binding = &scope->bindings[i];
        if (bindings) {
            if (!read_binding(reader, binding) || binding->creating_op.value == id.value) {
                return false;
            }
        } else {
            binding->creating_op = id;
            if (!read_channel(reader, &binding->channel)) {
                return false;
            }
        }
    }
    return true;
}

static bool read_node(core_cbor_reader *reader, core_node_request *request,
                      core_request_scope *scope, core_op_id id)
{
    const uint8_t *artifact_ref, *configuration;
    size_t artifact_ref_size, configuration_size;
    uint64_t dynamic;
    if (!core_cbor_uint(reader, &request->logical_node_id) ||
        !core_cbor_bytes(reader, &artifact_ref, &artifact_ref_size) ||
        artifact_ref_size == 0 || artifact_ref_size > CORE_MAX_ARTIFACT_REF_BYTES ||
        !read_u32(reader, &request->artifact_bytes) || request->artifact_bytes == 0 ||
        request->artifact_bytes > CORE_MAX_ARTIFACT_BYTES ||
        !read_u32(reader, &request->artifact_crc32) ||
        !read_u32(reader, &request->required_bytes) || request->required_bytes < request->artifact_bytes ||
        !core_cbor_bytes(reader, &configuration, &configuration_size) ||
        configuration_size > CORE_MAX_CONFIGURATION_BYTES ||
        !core_cbor_uint(reader, &request->addition_allowance_us) ||
        !core_cbor_uint(reader, &request->cleanup_allowance_us) ||
        !read_ports(reader, scope) || !core_cbor_uint(reader, &dynamic) || dynamic > 1) {
        return false;
    }
    request->artifact_ref_size = (uint8_t)artifact_ref_size;
    request->configuration_size = (uint8_t)configuration_size;
    memcpy(request->artifact_ref, artifact_ref, artifact_ref_size);
    memcpy(request->configuration, configuration, configuration_size);
    scope->dynamic_ports = dynamic != 0;
    scope->logical_node_id = request->logical_node_id;
    scope->instance = (core_instance_id){id.value};
    scope->allowance_us = request->addition_allowance_us;
    scope->cleanup_allowance_us = request->cleanup_allowance_us;
    scope->object_count = 1;
    return true;
}

static bool read_predicate(core_cbor_reader *reader, core_request_scope *scope)
{
    const uint8_t *bytes;
    size_t size;
    if (!core_cbor_bytes(reader, &bytes, &size) || size == 0 ||
        size % 2 != 0 || size / 2 > CORE_MAX_PREDICATE_TOKENS) {
        return false;
    }
    core_predicate_token tokens[CORE_MAX_PREDICATE_TOKENS];
    for (size_t i = 0; i < size / 2; ++i) {
        tokens[i] = (core_predicate_token){.kind = bytes[2 * i], .channel = bytes[2 * i + 1]};
        if (tokens[i].kind != CORE_PREDICATE_CHANNEL && tokens[i].channel != 0) {
            return false; /* one representation for a non-channel token */
        }
    }
    return core_predicate_init(&scope->predicate, tokens, size / 2, scope->channel_count);
}

static bool read_request(core_cbor_reader *reader, core_op_id id,
                         core_node_request *request, core_request_scope *scope)
{
    uint64_t count;
    if (!core_cbor_array(reader, &count)) {
        return false;
    }
    switch (scope->kind) {
    case CORE_REQUEST_ADD_NODE_NO_EDGES:
    case CORE_REQUEST_ADD_NODE:
        if (count != (scope->kind == CORE_REQUEST_ADD_NODE ? 12 : 10) ||
            !read_node(reader, request, scope, id)) {
            return false;
        }
        if (scope->kind == CORE_REQUEST_ADD_NODE) {
            if (!read_channels(reader, scope, id, false) || !read_predicate(reader, scope)) {
                return false;
            }
        } else {
            const core_predicate_token always = {.kind = CORE_PREDICATE_TRUE};
            if (!core_predicate_init(&scope->predicate, &always, 1, 0)) {
                return false;
            }
        }
        break;
    case CORE_REQUEST_ADD_EDGE:
        scope->channel_count = 1;
        scope->bindings[0].creating_op = id;
        if (count != 2 || !read_channel(reader, &scope->bindings[0].channel) ||
            !core_cbor_uint(reader, &scope->cleanup_allowance_us)) {
            return false;
        }
        scope->allowance_us = scope->bindings[0].channel.addition_allowance_us;
        break;
    case CORE_REQUEST_REMOVE_NODE_NO_EDGES:
    case CORE_REQUEST_REMOVE_NODE:
        if (count != (scope->kind == CORE_REQUEST_REMOVE_NODE ? 4 : 3) ||
            !core_cbor_uint(reader, &scope->logical_node_id) ||
            !core_cbor_uint(reader, &scope->instance.value) || scope->instance.value == id.value ||
            !core_cbor_uint(reader, &scope->allowance_us)) {
            return false;
        }
        scope->object_count = 1;
        if (scope->kind == CORE_REQUEST_REMOVE_NODE && !read_channels(reader, scope, id, true)) {
            return false;
        }
        break;
    case CORE_REQUEST_REMOVE_EDGE:
        scope->channel_count = 1;
        if (count != 2 || !read_binding(reader, &scope->bindings[0]) ||
            scope->bindings[0].creating_op.value == id.value ||
            !core_cbor_uint(reader, &scope->allowance_us)) {
            return false;
        }
        break;
    default:
        return false;
    }
    scope->object_count = (uint8_t)(scope->object_count + 2 * scope->channel_count);
    return reader->offset == reader->size;
}

static core_admit_result submit(core_admission *pool, uint64_t sender,
                                const uint8_t *data, size_t size, uint64_t now_us,
                                const core_admission_record **out, bool trusted_node,
                                bool endpoint_only, bool cancel)
{
    if (data == NULL || size == 0 || size > CORE_MAX_REQUEST_BYTES) {
        return CORE_ADMIT_INVALID;
    }
    core_cbor_reader envelope = {.data = data, .size = size};
    uint64_t count, version, kind;
    core_op_id id;
    const uint8_t *body;
    size_t body_size;
    if (!core_cbor_array(&envelope, &count) || count != 4 ||
        !core_cbor_uint(&envelope, &version) || version != CORE_SCHEMA_VERSION ||
        !core_cbor_uint(&envelope, &kind) || kind < CORE_REQUEST_ADD_NODE_NO_EDGES ||
        kind > CORE_REQUEST_REMOVE_EDGE ||
        (trusted_node && kind != CORE_REQUEST_ADD_NODE_NO_EDGES) ||
        !core_cbor_uint(&envelope, &id.value) ||
        !core_cbor_bytes(&envelope, &body, &body_size) || envelope.offset != size) {
        return CORE_ADMIT_INVALID;
    }
    if (!trusted_node && !endpoint_only && (!pool->configured || sender != pool->authority)) {
        return CORE_ADMIT_UNAUTHORIZED;
    }
    if (!endpoint_only && core_admission_find(pool, id) != NULL) {
        return CORE_ADMIT_DUPLICATE;
    }
    core_cbor_reader reader = {.data = body, .size = body_size};
    core_node_request request = {0};
    core_request_scope scope = {.kind = (uint8_t)kind};
    if (!read_request(&reader, id, &request, &scope)) {
        return CORE_ADMIT_INVALID;
    }
    scope.coordinator = pool->local_device;
    if (scope.kind == CORE_REQUEST_ADD_EDGE || scope.kind == CORE_REQUEST_REMOVE_EDGE) {
        scope.coordinator = scope.bindings[0].channel.source_owner;
    } else if (endpoint_only && scope.channel_count > 0) {
        const core_channel *channel = &scope.bindings[0].channel;
        scope.coordinator = channel->source_instance.value == scope.instance.value ?
                            channel->source_owner : channel->destination_owner;
    }
    if (cancel && scope.kind != CORE_REQUEST_ADD_NODE && scope.kind != CORE_REQUEST_ADD_EDGE) {
        return CORE_ADMIT_INVALID;
    }
    if (endpoint_only) {
        bool known_sender = false;
        for (size_t i = 0; i < pool->peer_count; ++i) {
            known_sender |= pool->peers[i] == sender;
        }
        if (!pool->configured || !known_sender || sender != scope.coordinator ||
            scope.channel_count == 0) {
            return CORE_ADMIT_UNAUTHORIZED;
        }
        const core_admission_record *existing = core_admission_find(pool, id);
        if (existing != NULL) {
            if (cancel) {
                if (existing->scope.coordinator != sender) {
                    return CORE_ADMIT_UNAUTHORIZED;
                }
                (void)core_admission_close_scope(pool, id);
                *out = existing;
            }
            return CORE_ADMIT_DUPLICATE;
        }
    } else if (scope.coordinator != pool->local_device) {
        return CORE_ADMIT_UNAUTHORIZED;
    }
    return core_admission_reserve(pool, id, &request, &scope, now_us, endpoint_only, cancel, out);
}

core_admit_result core_request_submit(core_admission *pool, uint64_t sender,
                                      const uint8_t *data, size_t size,
                                      uint64_t now_us, const core_admission_record **out)
{
    return submit(pool, sender, data, size, now_us, out, false, false, false);
}

core_admit_result core_node_submit(core_admission *pool, const uint8_t *data,
                                   size_t size, uint64_t now_us,
                                   const core_admission_record **out)
{
    return submit(pool, 0, data, size, now_us, out, true, false, false);
}

core_admit_result core_endpoint_submit(core_admission *pool, uint64_t coordinator,
                                       const uint8_t *data, size_t size,
                                       uint64_t now_us, const core_admission_record **out)
{
    return submit(pool, coordinator, data, size, now_us, out, false, true, false);
}

core_admit_result core_endpoint_cancel(core_admission *pool, uint64_t coordinator,
                                       const uint8_t *data, size_t size,
                                       uint64_t now_us, const core_admission_record **out)
{
    return submit(pool, coordinator, data, size, now_us, out, false, true, true);
}

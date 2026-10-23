#ifndef ADMISSION_FIXTURE_H
#define ADMISSION_FIXTURE_H
#include "kaos_admission.h"
#include "../../../graph_core/kaos_cbor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct {
    core_op_id id;
    core_node_request node;
    core_request_scope scope;
} fixture_request;

static inline fixture_request fixture_node(uint64_t id, uint64_t logical)
{
    fixture_request request = {.id = {id},
        .node = {.logical_node_id = logical, .artifact_bytes = 16, .required_bytes = 128,
                 .artifact_ref_size = 1, .artifact_ref = {'a'},
                 .addition_allowance_us = 100, .cleanup_allowance_us = 50},
        .scope = {.kind = CORE_REQUEST_ADD_NODE_NO_EDGES, .port_count = 2,
                  .ports = {{.id = 1, .direction = CORE_PORT_OUTPUT, .type = 3, .max_payload = 64},
                            {.id = 2, .direction = CORE_PORT_INPUT, .type = 3, .max_payload = 64}}}};
    return request;
}

static inline core_channel fixture_channel(uint64_t source, uint64_t destination,
                                            uint64_t source_instance, uint64_t destination_instance,
                                            uint64_t source_owner, uint64_t destination_owner)
{
    return (core_channel){.source_logical = source, .destination_logical = destination,
        .service = 7, .source_instance = {source_instance}, .destination_instance = {destination_instance},
        .source_owner = source_owner, .destination_owner = destination_owner,
        .source_port = 1, .destination_port = 2, .type = 3,
        .payload_bytes = 16, .queue_messages = 2, .addition_allowance_us = 100};
}

static inline void fixture_write_channel(core_cbor_writer *writer, const core_channel *channel)
{
    CHECK(core_cbor_write_array(writer, 13));
    CHECK(core_cbor_write_uint(writer, channel->source_logical));
    CHECK(core_cbor_write_uint(writer, channel->destination_logical));
    CHECK(core_cbor_write_uint(writer, channel->service));
    CHECK(core_cbor_write_uint(writer, channel->source_instance.value));
    CHECK(core_cbor_write_uint(writer, channel->destination_instance.value));
    CHECK(core_cbor_write_uint(writer, channel->source_owner));
    CHECK(core_cbor_write_uint(writer, channel->destination_owner));
    CHECK(core_cbor_write_uint(writer, channel->source_port));
    CHECK(core_cbor_write_uint(writer, channel->destination_port));
    CHECK(core_cbor_write_uint(writer, channel->type));
    CHECK(core_cbor_write_uint(writer, channel->payload_bytes));
    CHECK(core_cbor_write_uint(writer, channel->queue_messages));
    CHECK(core_cbor_write_uint(writer, channel->addition_allowance_us));
}

static inline void fixture_write_binding(core_cbor_writer *writer, const core_binding *binding)
{
    CHECK(core_cbor_write_array(writer, 2));
    fixture_write_channel(writer, &binding->channel);
    CHECK(core_cbor_write_uint(writer, binding->creating_op.value));
}

static inline size_t fixture_envelope(uint8_t *data, uint8_t kind, uint64_t id,
                                      const uint8_t *body, size_t body_size)
{
    core_cbor_writer writer = {.data = data, .size = CORE_MAX_REQUEST_BYTES};
    CHECK(core_cbor_write_array(&writer, 4));
    CHECK(core_cbor_write_uint(&writer, CORE_SCHEMA_VERSION));
    CHECK(core_cbor_write_uint(&writer, kind));
    CHECK(core_cbor_write_uint(&writer, id));
    CHECK(core_cbor_write_bytes(&writer, body, body_size));
    return writer.offset;
}

static inline size_t fixture_encode(const fixture_request *request, uint8_t *data)
{
    uint8_t body[CORE_MAX_REQUEST_BYTES];
    core_cbor_writer writer = {.data = body, .size = sizeof(body)};
    const core_request_scope *scope = &request->scope;
    const core_node_request *node = &request->node;
    if (scope->kind == CORE_REQUEST_ADD_NODE || scope->kind == CORE_REQUEST_ADD_NODE_NO_EDGES) {
        CHECK(core_cbor_write_array(&writer, scope->kind == CORE_REQUEST_ADD_NODE ? 11 : 9));
        CHECK(core_cbor_write_uint(&writer, node->logical_node_id));
        CHECK(core_cbor_write_bytes(&writer, node->artifact_ref, node->artifact_ref_size));
        CHECK(core_cbor_write_uint(&writer, node->artifact_bytes));
        CHECK(core_cbor_write_uint(&writer, node->artifact_crc32));
        CHECK(core_cbor_write_uint(&writer, node->required_bytes));
        CHECK(core_cbor_write_bytes(&writer, node->configuration, node->configuration_size));
        CHECK(core_cbor_write_uint(&writer, node->addition_allowance_us));
        CHECK(core_cbor_write_uint(&writer, node->cleanup_allowance_us));
        CHECK(core_cbor_write_array(&writer, scope->port_count));
        for (size_t i = 0; i < scope->port_count; ++i) {
            const core_port *port = &scope->ports[i];
            CHECK(core_cbor_write_array(&writer, 4));
            CHECK(core_cbor_write_uint(&writer, port->id));
            CHECK(core_cbor_write_uint(&writer, port->direction));
            CHECK(core_cbor_write_uint(&writer, port->type));
            CHECK(core_cbor_write_uint(&writer, port->max_payload));
        }
        if (scope->kind == CORE_REQUEST_ADD_NODE) {
            CHECK(core_cbor_write_array(&writer, scope->channel_count));
            for (size_t i = 0; i < scope->channel_count; ++i) {
                fixture_write_channel(&writer, &scope->bindings[i].channel);
            }
            uint8_t tokens[CORE_MAX_PREDICATE_TOKENS * 2];
            for (size_t i = 0; i < scope->predicate.count; ++i) {
                tokens[2 * i] = scope->predicate.tokens[i].kind;
                tokens[2 * i + 1] = scope->predicate.tokens[i].channel;
            }
            CHECK(core_cbor_write_bytes(&writer, tokens, 2 * scope->predicate.count));
        }
    } else if (scope->kind == CORE_REQUEST_ADD_EDGE || scope->kind == CORE_REQUEST_REMOVE_EDGE) {
        CHECK(core_cbor_write_array(&writer, 2));
        if (scope->kind == CORE_REQUEST_ADD_EDGE) {
            fixture_write_channel(&writer, &scope->bindings[0].channel);
            CHECK(core_cbor_write_uint(&writer, scope->cleanup_allowance_us));
        } else {
            fixture_write_binding(&writer, &scope->bindings[0]);
            CHECK(core_cbor_write_uint(&writer, scope->allowance_us));
        }
    } else {
        CHECK(core_cbor_write_array(&writer, scope->kind == CORE_REQUEST_REMOVE_NODE ? 4 : 3));
        CHECK(core_cbor_write_uint(&writer, scope->logical_node_id));
        CHECK(core_cbor_write_uint(&writer, scope->instance.value));
        CHECK(core_cbor_write_uint(&writer, scope->allowance_us));
        if (scope->kind == CORE_REQUEST_REMOVE_NODE) {
            CHECK(core_cbor_write_array(&writer, scope->channel_count));
            for (size_t i = 0; i < scope->channel_count; ++i) {
                fixture_write_binding(&writer, &scope->bindings[i]);
            }
        }
    }
    return fixture_envelope(data, scope->kind, request->id.value, body, writer.offset);
}

static inline void fixture_pool(core_admission *pool, uint64_t local)
{
    static const uint64_t peers[] = {1, 2, 3};
    CHECK(core_admission_init(pool, 4096, 2 * 4096 + 8 * CORE_ENDPOINT_BYTES));
    CHECK(core_admission_configure(pool, local, 99, peers, 3));
}

static inline core_admit_result fixture_submit(core_admission *pool, const fixture_request *request,
                                               const core_admission_record **out)
{
    uint8_t data[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(request, data);
    return core_request_submit(pool, 99, data, size, 10, out);
}
#endif

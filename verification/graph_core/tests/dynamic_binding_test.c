#include "kaos_admission.h"
#include "../../../graph_core/kaos_cbor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static size_t envelope(uint8_t *out, uint8_t kind, uint64_t id,
                        const uint8_t *body, size_t size)
{
    core_cbor_writer w = {.data = out, .size = CORE_MAX_REQUEST_BYTES};
    core_cbor_write_array(&w, 4);
    core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
    core_cbor_write_uint(&w, kind);
    core_cbor_write_uint(&w, id);
    core_cbor_write_bytes(&w, body, size);
    CHECK(!w.failed);
    return w.offset;
}

static size_t node_request(uint8_t *out, uint64_t id, bool source, int legacy_flag)
{
    uint8_t body[128];
    core_cbor_writer w = {.data = body, .size = sizeof(body)};
    core_cbor_write_array(&w, legacy_flag < 0 ? 9 : 10);
    core_cbor_write_uint(&w, source ? 10 : 20);
    core_cbor_write_bytes(&w, (const uint8_t *)"m", 1);
    core_cbor_write_uint(&w, 8);
    core_cbor_write_uint(&w, 0);
    core_cbor_write_uint(&w, 128);
    core_cbor_write_bytes(&w, NULL, 0);
    core_cbor_write_uint(&w, 100);
    core_cbor_write_uint(&w, 50);
    core_cbor_write_array(&w, 1);
    core_cbor_write_array(&w, 4);
    core_cbor_write_uint(&w, source ? 1 : 2);
    core_cbor_write_uint(&w, source ? CORE_PORT_OUTPUT : CORE_PORT_INPUT);
    core_cbor_write_uint(&w, 7);
    core_cbor_write_uint(&w, 64);
    if (legacy_flag >= 0) core_cbor_write_uint(&w, (uint64_t)legacy_flag);
    CHECK(!w.failed);
    return envelope(out, CORE_REQUEST_ADD_NODE_NO_EDGES, id, body, w.offset);
}

static size_t edge_request(uint8_t *out, uint64_t id, uint16_t source_port)
{
    uint8_t body[128];
    core_cbor_writer w = {.data = body, .size = sizeof(body)};
    const uint64_t channel[] = {10, 20, id, 1, 2, 1, 1, source_port, 2, 7, 16, 2, 100};
    core_cbor_write_array(&w, 2);
    core_cbor_write_array(&w, 13);
    for (size_t i = 0; i < 13; ++i) core_cbor_write_uint(&w, channel[i]);
    core_cbor_write_uint(&w, 50);
    CHECK(!w.failed);
    return envelope(out, CORE_REQUEST_ADD_EDGE, id, body, w.offset);
}

static void reject(core_admission *pool, const uint8_t *wire, size_t size,
                   core_admit_result expected)
{
    const core_admission before = *pool;
    const core_admission_record *record = NULL;
    CHECK(core_request_submit(pool, 99, wire, size, 10, &record) == expected);
    CHECK(record == NULL && memcmp(pool, &before, sizeof(before)) == 0);
}

int main(void)
{
    core_admission pool;
    const uint64_t peers[] = {1};
    CHECK(core_admission_init(&pool, 4096, 16384));
    CHECK(core_admission_configure(&pool, 1, 99, peers, 1));
    uint8_t wire[CORE_MAX_REQUEST_BYTES];
    const core_admission_record *record;
    /* Neither old flag value is part of the new node configuration. */
    for (int flag = 0; flag <= 1; ++flag) {
        const size_t size = node_request(wire, 1, true, flag);
        reject(&pool, wire, size, CORE_ADMIT_INVALID);
    }
    size_t size = node_request(wire, 1, true, -1);
    wire[1] = 2;
    reject(&pool, wire, size, CORE_ADMIT_INVALID);
    wire[1] = CORE_SCHEMA_VERSION;
    CHECK(core_request_submit(&pool, 99, wire, size, 10, &record) == CORE_ADMIT_ACCEPTED);
    size = node_request(wire, 2, false, -1);
    CHECK(core_request_submit(&pool, 99, wire, size, 10, &record) == CORE_ADMIT_ACCEPTED);
    size = edge_request(wire, 3, 1);
    reject(&pool, wire, size, CORE_ADMIT_CONFLICT); /* establishment still required */
    CHECK(core_admission_establish_node(&pool, (core_instance_id){1}));
    CHECK(core_admission_establish_node(&pool, (core_instance_id){2}));
    size = edge_request(wire, 3, 9);
    reject(&pool, wire, size, CORE_ADMIT_CONFLICT); /* declared ports remain authoritative */
    size = edge_request(wire, 3, 1);
    CHECK(core_request_submit(&pool, 99, wire, size, 10, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(pool.endpoints[0].occupied && pool.endpoints[1].occupied);
    CHECK(core_admission_close_scope(&pool, (core_op_id){1}));
    size = edge_request(wire, 4, 1);
    reject(&pool, wire, size, CORE_ADMIT_CONFLICT); /* closing instances reject bindings */
    puts("dynamic binding: unconditional capability, strict schema and retained lifecycle/port guards passed");
    return 0;
}

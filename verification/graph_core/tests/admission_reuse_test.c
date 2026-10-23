#include "admission_fixture.h"
#include "kaos_edge.h"

static core_admit_result remove_binding(core_admission *pool, uint64_t owner,
    core_op_id id, uint8_t index, core_instance_id target, const core_binding *binding,
    const core_admission_record **out)
{
    uint8_t bytes[CORE_EDGE_MESSAGE_BYTES];
    core_cbor_writer w = {.data = bytes, .size = sizeof(bytes)};
    CHECK(core_cbor_write_array(&w, 6));
    CHECK(core_cbor_write_uint(&w, CORE_SCHEMA_VERSION));
    CHECK(core_cbor_write_uint(&w, CORE_REMOVAL_COMMAND_KIND));
    CHECK(core_cbor_write_uint(&w, id.value));
    CHECK(core_cbor_write_uint(&w, index));
    CHECK(core_cbor_write_uint(&w, target.value));
    fixture_write_binding(&w, binding);
    return core_endpoint_removal_submit(pool, owner, bytes, w.offset, out);
}

int main(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    core_endpoint_service endpoints;
    core_endpoint_init(&endpoints, &pool, NULL, NULL);
    core_edge_service edges = {.admission = &pool, .endpoints = &endpoints};
    const core_binding old = {fixture_channel(10, 20, 100, 20, 2, 1), {100}};
    const core_admission_record *out;
    /* Fill the pool with real sparse removal observers, then settle and deliver
     * their local closure. They remain fences, eligible for pressure eviction. */
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i)
        CHECK(remove_binding(&pool, 2, (core_op_id){200 + i}, 0,
            (core_instance_id){100}, &old, &out) == CORE_ADMIT_ACCEPTED);
    core_edge_step(&edges, 1);
    core_edge_effect effect;
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        CHECK(core_edge_next(&edges, 2, 1, &effect));
        CHECK(effect.kind == CORE_EDGE_OWNER);
        core_edge_delivered(&edges, &effect);
    }
    core_admission_maintain(&pool);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i)
        CHECK(pool.records[i].occupied && pool.records[i].owner_delivered);

    const core_admission before = pool;
    core_binding wrong = old;
    ++wrong.channel.payload_bytes;
    CHECK(remove_binding(&pool, 2, (core_op_id){500}, 0,
        (core_instance_id){100}, &wrong, &out) == CORE_ADMIT_CONFLICT);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);

    const core_binding fresh = {fixture_channel(10, 20, 300, 20, 2, 1), {300}};
    CHECK(remove_binding(&pool, 2, (core_op_id){500}, 0,
        (core_instance_id){300}, &fresh, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(out->id.value == 500 && out->incarnation == 2);
    const core_admission after = pool;
    CHECK(remove_binding(&pool, 2, (core_op_id){500}, 1,
        (core_instance_id){300}, &fresh, &out) == CORE_ADMIT_CONFLICT);
    CHECK(memcmp(&after, &pool, sizeof(pool)) == 0);
    puts("Retired observer reuse preserves binding validation");
    return 0;
}

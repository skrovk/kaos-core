#include "admission_fixture.h"

static size_t command(uint8_t *bytes, uint64_t id, uint8_t index,
                       const core_channel *channel)
{
    core_cbor_writer writer = {.data = bytes, .size = 160};
    CHECK(core_cbor_write_array(&writer, 5));
    CHECK(core_cbor_write_uint(&writer, CORE_SCHEMA_VERSION));
    CHECK(core_cbor_write_uint(&writer, 12));
    CHECK(core_cbor_write_uint(&writer, id));
    CHECK(core_cbor_write_uint(&writer, index));
    fixture_write_channel(&writer, channel);
    return writer.offset;
}

static core_admit_result submit(core_admission *pool, uint64_t sender, uint64_t id,
                                 uint8_t index, const core_channel *channel, bool cancel,
                                 const core_admission_record **out)
{
    uint8_t bytes[160];
    const size_t size = command(bytes, id, index, channel);
    /* A Ki clock near exhaustion cannot expire Kc's independent allowance. */
    return core_endpoint_channel_submit(pool, sender, bytes, size, UINT64_MAX, cancel, out);
}

static size_t endpoint_count(const core_admission *pool)
{
    size_t count = 0;
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i)
        count += pool->endpoints[i].occupied;
    return count;
}

static void sparse_independent_work(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    core_channel a = fixture_channel(100, 20, 100, 20, 2, 1);
    core_channel b = a;
    core_channel c = a;
    b.service = 8;
    c.service = 9;
    const core_admission_record *out;
    CHECK(submit(&pool, 2, 100, 2, &c, true, &out) == CORE_ADMIT_ACCEPTED);
    const core_admission_record *owner = out;
    CHECK(owner->channel_known == 4 && owner->channel_cleanup == 4);
    CHECK(!owner->cleanup_selected && owner->endpoint_only && owner->deadline_us == 0);
    CHECK(endpoint_count(&pool) == 0 && pool.reserved_bytes == 0);
    CHECK(submit(&pool, 2, 100, 2, &c, false, &out) == CORE_ADMIT_DUPLICATE);
    CHECK(endpoint_count(&pool) == 0);

    const fixture_request local = fixture_node(20, 20);
    CHECK(fixture_submit(&pool, &local, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){20}));
    CHECK(submit(&pool, 2, 100, 0, &a, false, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(out == owner);
    CHECK(submit(&pool, 2, 100, 1, &b, false, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(out == owner && owner->channel_known == 7 && owner->channel_cleanup == 4);
    CHECK(endpoint_count(&pool) == 2);
    CHECK(pool.reserved_bytes == pool.node_bytes + 2 * CORE_ENDPOINT_BYTES);
    core_admission before = pool;
    CHECK(submit(&pool, 2, 100, 0, &a, false, &out) == CORE_ADMIT_DUPLICATE);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);

    core_channel wrong = a;
    wrong.payload_bytes = 17;
    out = NULL;
    CHECK(submit(&pool, 2, 100, 0, &wrong, false, &out) == CORE_ADMIT_CONFLICT);
    CHECK(out == NULL && memcmp(&before, &pool, sizeof(pool)) == 0);
    CHECK(submit(&pool, 3, 100, 0, &a, true, &out) == CORE_ADMIT_UNAUTHORIZED);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);

    wrong = a;
    wrong.service = 10;
    wrong.destination_port = 99;
    CHECK(submit(&pool, 2, 100, 3, &wrong, false, &out) == CORE_ADMIT_CONFLICT);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);
    CHECK(submit(&pool, 2, 100, 3, &wrong, true, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(owner->channel_known == 15 && owner->channel_cleanup == 12);
    CHECK(core_admission_close_channel(&pool, (core_op_id){100}, 0));
    CHECK(owner->channel_cleanup == 13 && !owner->cleanup_selected);
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        const core_reserved_endpoint *endpoint = &pool.endpoints[i];
        if (!endpoint->occupied) continue;
        CHECK(endpoint->lifetime.closing == (endpoint->binding.channel.service == 7));
    }
    CHECK(pool.nodes[0].established && !pool.nodes[0].lifetime.closing);
    CHECK(core_admission_close_scope(&pool, (core_op_id){100}));
    CHECK(owner->cleanup_selected && owner->channel_cleanup == owner->channel_known);
    CHECK(pool.nodes[0].established && !pool.nodes[0].lifetime.closing);
}

static void cancel_with_full_resources(void)
{
    core_admission pool;
    static const uint64_t peers[] = {1, 2};
    CHECK(core_admission_init(&pool, 4096, 4096));
    CHECK(core_admission_configure(&pool, 1, 99, peers, 2));
    const fixture_request local = fixture_node(20, 20);
    const core_admission_record *out;
    CHECK(fixture_submit(&pool, &local, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){20}));
    core_channel c = fixture_channel(100, 20, 100, 20, 2, 1);
    core_admission before = pool;
    CHECK(submit(&pool, 2, 100, 0, &c, false, &out) == CORE_ADMIT_FULL);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);
    CHECK(submit(&pool, 2, 100, 0, &c, true, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(out->channel_cleanup == 1 && endpoint_count(&pool) == 0);
    CHECK(submit(&pool, 2, 100, 0, &c, false, &out) == CORE_ADMIT_DUPLICATE);
    CHECK(endpoint_count(&pool) == 0 && pool.reserved_bytes == 4096);
}

static void incoming_new_node(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const fixture_request local = fixture_node(20, 20);
    const core_admission_record *out;
    CHECK(fixture_submit(&pool, &local, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&pool, (core_instance_id){20}));
    const core_channel c = fixture_channel(20, 100, 20, 100, 1, 2);
    CHECK(submit(&pool, 2, 100, CORE_MAX_REQUEST_CHANNELS - 1, &c, false, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(out->scope.logical_node_id == 100 && out->scope.instance.value == 100);
    CHECK(out->channel_known == (1u << (CORE_MAX_REQUEST_CHANNELS - 1)));
    CHECK(pool.endpoints[0].occupied && pool.endpoints[0].sender);
    uint8_t bytes[160];
    const size_t size = command(bytes, 100, CORE_MAX_REQUEST_CHANNELS, &c);
    core_admission before = pool;
    CHECK(core_endpoint_channel_submit(&pool, 2, bytes, size, 0, false, &out) == CORE_ADMIT_INVALID);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);
}

int main(void)
{
    sparse_independent_work();
    cancel_with_full_resources();
    incoming_new_node();
    puts("compact channel admission passed");
    return 0;
}

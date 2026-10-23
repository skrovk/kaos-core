#include "admission_fixture.h"

static void rejected(core_admission *pool, const uint8_t *data, size_t size,
                      core_admit_result expected)
{
    const core_admission before = *pool;
    const core_admission_record *out = &pool->records[CORE_MAX_ADMISSION_RECORDS - 1];
    const core_admission_record *sentinel = out;
    CHECK(core_request_submit(pool, 99, data, size, 10, &out) == expected);
    CHECK(out == sentinel && memcmp(pool, &before, sizeof(*pool)) == 0);
}

static void rejected_request(core_admission *pool, const fixture_request *request,
                              core_admit_result expected)
{
    uint8_t data[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(request, data);
    rejected(pool, data, size, expected);
}

static void strict_envelope_and_owned_copy(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    fixture_request request = fixture_node(1, 10);
    request.node.configuration_size = 2;
    request.node.configuration[0] = 4;
    request.node.configuration[1] = 5;
    uint8_t data[CORE_MAX_REQUEST_BYTES + 1];
    const size_t size = fixture_encode(&request, data);
    for (size_t cut = 0; cut < size; ++cut) {
        rejected(&pool, data, cut, CORE_ADMIT_INVALID);
    }
    rejected(&pool, NULL, size, CORE_ADMIT_INVALID);
    rejected(&pool, data, sizeof(data), CORE_ADMIT_INVALID);
    uint8_t original[CORE_MAX_REQUEST_BYTES];
    memcpy(original, data, size);
    data[size] = 0;
    rejected(&pool, data, size + 1, CORE_ADMIT_INVALID);
    data[0] = 0x9f; /* indefinite envelope */
    rejected(&pool, data, size, CORE_ADMIT_INVALID);
    memcpy(data, original, size);
    data[1] = 1;
    rejected(&pool, data, size, CORE_ADMIT_INVALID);
    memcpy(data, original, size);
    memmove(data + 3, data + 2, size - 2);
    data[1] = 0x18;
    data[2] = CORE_SCHEMA_VERSION; /* non-shortest version */
    rejected(&pool, data, size + 1, CORE_ADMIT_INVALID);
    memcpy(data, original, size);
    const core_admission before = pool;
    const core_admission_record *out = NULL;
    CHECK(core_request_submit(&pool, 98, data, size, 10, &out) == CORE_ADMIT_UNAUTHORIZED);
    CHECK(out == NULL && memcmp(&pool, &before, sizeof(pool)) == 0);
    CHECK(core_request_submit(&pool, 99, data, size, 10, &out) == CORE_ADMIT_ACCEPTED);
    memset(data, 0, size);
    CHECK(out->request.artifact_ref[0] == 'a' && out->request.configuration[0] == 4);
    CHECK(out->request.configuration[1] == 5 && out->scope.ports[0].direction == CORE_PORT_OUTPUT);
    CHECK(out->node->port_count == 2 && out->deadline_us == 110);
    const uint8_t bad_body[] = {0xff};
    size_t duplicate_size = fixture_envelope(data, CORE_REQUEST_REMOVE_EDGE, 1, bad_body, 1);
    const core_admission accepted = pool;
    const core_admission_record *unchanged = out;
    CHECK(core_request_submit(&pool, 99, data, duplicate_size, UINT64_MAX, &out) == CORE_ADMIT_DUPLICATE);
    CHECK(out == unchanged && memcmp(&pool, &accepted, sizeof(pool)) == 0);
    data[1] = 1; /* retained id does not bypass version framing */
    rejected(&pool, data, duplicate_size, CORE_ADMIT_INVALID);
    duplicate_size = fixture_envelope(data, CORE_REQUEST_ADD_NODE_NO_EDGES, 2, bad_body, 1);
    rejected(&pool, data, duplicate_size, CORE_ADMIT_INVALID);
}

static void body_and_manifest_boundaries(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    fixture_request request = fixture_node(1, 10);
    request.node.artifact_bytes = 0;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = fixture_node(1, 10);
    request.node.artifact_bytes = CORE_MAX_ARTIFACT_BYTES + 1;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = fixture_node(1, 10);
    request.node.required_bytes = 15;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request.node.required_bytes = 4097;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = fixture_node(1, 10);
    request.node.artifact_ref_size = 0;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = fixture_node(1, 10);
    request.node.addition_allowance_us = 0;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request.node.addition_allowance_us = UINT64_MAX;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = fixture_node(1, 10);
    request.node.cleanup_allowance_us = UINT64_MAX - 100;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = fixture_node(1, 10);
    request.scope.ports[1].id = 1;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = fixture_node(1, 10);
    request.scope.ports[0].direction = 2;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request.scope.ports[0].direction = CORE_PORT_OUTPUT;
    request.scope.ports[0].max_payload = CORE_MAX_PAYLOAD_BYTES + 1;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);

    /* Exact maximum artifact/reference/configuration values remain valid with
     * a sufficiently large explicitly configured node quota. */
    CHECK(core_admission_init(&pool, CORE_MAX_ARTIFACT_BYTES, CORE_MAX_ARTIFACT_BYTES));
    const uint64_t peers[] = {1};
    CHECK(core_admission_configure(&pool, 1, 99, peers, 1));
    request = fixture_node(UINT64_MAX, UINT64_MAX);
    request.node.artifact_ref_size = CORE_MAX_ARTIFACT_REF_BYTES;
    memset(request.node.artifact_ref, 0x42, sizeof(request.node.artifact_ref));
    request.node.configuration_size = CORE_MAX_CONFIGURATION_BYTES;
    memset(request.node.configuration, 0x43, sizeof(request.node.configuration));
    request.node.artifact_bytes = CORE_MAX_ARTIFACT_BYTES;
    request.node.required_bytes = CORE_MAX_ARTIFACT_BYTES;
    request.node.artifact_crc32 = UINT32_MAX;
    const core_admission_record *out;
    CHECK(fixture_submit(&pool, &request, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(out->id.value == UINT64_MAX && out->node->instance.value == UINT64_MAX);
    CHECK(out->request.artifact_bytes == CORE_MAX_ARTIFACT_BYTES);
}

static void channel_and_predicate_validation(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    fixture_request request = fixture_node(1, 10);
    request.scope.kind = CORE_REQUEST_ADD_NODE;
    request.scope.channel_count = 1;
    request.scope.bindings[0].channel = fixture_channel(10, 20, 1, 2, 1, 2);
    request.scope.predicate.count = 1;
    request.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_FALSE, 0};
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_CHANNEL, 1};
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_TRUE, 1};
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request.scope.predicate.tokens[0] = (core_predicate_token){CORE_PREDICATE_TRUE, 0};
    const fixture_request valid = request;
    request.scope.bindings[0].channel.destination_logical = 10;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = valid;
    request.scope.bindings[0].channel.payload_bytes = 65;
    rejected_request(&pool, &request, CORE_ADMIT_CONFLICT);
    request = valid;
    request.scope.bindings[0].channel.queue_messages = CORE_MAX_QUEUE_MESSAGES + 1;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = valid;
    request.scope.bindings[0].channel.type = 4;
    rejected_request(&pool, &request, CORE_ADMIT_CONFLICT);
    request = valid;
    request.scope.bindings[0].channel.source_port = 2;
    rejected_request(&pool, &request, CORE_ADMIT_CONFLICT);
    request = valid;
    request.scope.bindings[0].channel.destination_owner = 98;
    rejected_request(&pool, &request, CORE_ADMIT_UNAUTHORIZED);
    request = valid;
    request.scope.bindings[0].channel.addition_allowance_us = 99;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = valid;
    request.scope.bindings[0].channel.source_instance.value = 3;
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = valid;
    request.scope.channel_count = 2;
    request.scope.bindings[1] = request.scope.bindings[0];
    rejected_request(&pool, &request, CORE_ADMIT_INVALID);
    request = valid;
    const core_admission_record *out;
    CHECK(fixture_submit(&pool, &request, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(out->scope.channel_count == 1 && out->scope.bindings[0].creating_op.value == 1);
    CHECK(core_predicate_eval(&out->scope.predicate, 0));
}

int main(void)
{
    strict_envelope_and_owned_copy();
    body_and_manifest_boundaries();
    channel_and_predicate_validation();
    puts("requests: deterministic framing, immutable copies, identity/manifest/predicate/deadline boundaries passed");
    return 0;
}

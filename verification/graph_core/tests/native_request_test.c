#include "admission_fixture.h"
#include "kaos_node.h"

/* Extend the old fixture's complete body, without changing that fixture or any
 * old regression. names is an already encoded CBOR array, including malformed
 * alternatives used below. */
static size_t native_request(const fixture_request *request, const uint8_t *names,
                             size_t names_size, uint8_t data[CORE_MAX_REQUEST_BYTES])
{
    uint8_t original[CORE_MAX_REQUEST_BYTES], extended[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(request, original);
    core_cbor_reader reader = {.data = original, .size = size};
    uint64_t ignored;
    const uint8_t *body;
    size_t body_size;
    CHECK(core_cbor_array(&reader, &ignored));
    CHECK(core_cbor_uint(&reader, &ignored));
    CHECK(core_cbor_uint(&reader, &ignored));
    CHECK(core_cbor_uint(&reader, &ignored));
    CHECK(core_cbor_bytes(&reader, &body, &body_size));
    CHECK(body_size + names_size <= sizeof(extended));
    CHECK(body[0] == 0x89 || body[0] == 0x8b);
    memcpy(extended, body, body_size);
    ++extended[0];
    memcpy(extended + body_size, names, names_size);
    return fixture_envelope(data, request->scope.kind, request->id.value,
                            extended, body_size + names_size);
}

static size_t name_list(uint8_t *data, const char *const names[], const size_t sizes[],
                        size_t count)
{
    core_cbor_writer writer = {.data = data, .size = 256};
    CHECK(core_cbor_write_array(&writer, count));
    for (size_t i = 0; i < count; ++i)
        CHECK(core_cbor_write_bytes(&writer, (const uint8_t *)names[i], sizes[i]));
    return writer.offset;
}

static size_t one_name(uint8_t *data, const char *name)
{
    const char *names[] = {name};
    const size_t sizes[] = {strlen(name)};
    return name_list(data, names, sizes, 1);
}

static void accepted_declarations(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    uint8_t names[256], wire[CORE_MAX_REQUEST_BYTES];
    const size_t names_size = one_name(names, "fixture_temperature_read");
    fixture_request request = fixture_node(1, 10);
    request.node.configuration_size = 3;
    memcpy(request.node.configuration, "cfg", 3);
    size_t size = native_request(&request, names, names_size, wire);
    const core_admission_record *record;
    CHECK(core_request_submit(&pool, 2, wire, size, 10, &record) == CORE_ADMIT_UNAUTHORIZED);
    CHECK(pool.reserved_bytes == 0);
    CHECK(core_request_submit(&pool, 99, wire, size, 10, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->request.imports.count == 1);
    CHECK(strcmp(record->request.imports.names[0], "fixture_temperature_read") == 0);
    CHECK(record->request.configuration_size == 3);
    CHECK(memcmp(record->request.configuration, "cfg", 3) == 0);
    const size_t reserved = pool.reserved_bytes;
    CHECK(core_request_submit(&pool, 99, wire, size, 10, &record) == CORE_ADMIT_DUPLICATE);
    CHECK(pool.reserved_bytes == reserved);
    memset(wire, 0xff, size);
    CHECK(strcmp(record->request.imports.names[0], "fixture_temperature_read") == 0);

    /* The trailing declaration is after both channel scope and predicate. */
    fixture_pool(&pool, 1);
    request.scope.kind = CORE_REQUEST_ADD_NODE;
    request.scope.channel_count = 1;
    request.scope.bindings[0].channel = fixture_channel(20, 10, 30, 1, 2, 1);
    request.scope.predicate.count = 1;
    request.scope.predicate.tokens[0] = (core_predicate_token){
        .kind = CORE_PREDICATE_CHANNEL, .channel = 0};
    size = native_request(&request, names, names_size, wire);
    CHECK(core_request_submit(&pool, 99, wire, size, 10, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->scope.channel_count == 1 && record->scope.predicate.count == 1);
    CHECK(record->request.imports.count == 1);
    CHECK(strcmp(record->request.imports.names[0], "fixture_temperature_read") == 0);
}

static void rejected(const uint8_t *names, size_t names_size)
{
    core_admission pool, before;
    fixture_pool(&pool, 1);
    before = pool;
    fixture_request request = fixture_node(1, 10);
    uint8_t wire[CORE_MAX_REQUEST_BYTES];
    const size_t size = native_request(&request, names, names_size, wire);
    const core_admission_record *record;
    CHECK(core_request_submit(&pool, 99, wire, size, 10, &record) == CORE_ADMIT_INVALID);
    CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);
}

static void malformed_declarations(void)
{
    uint8_t names[256];
    const char *invalid[] = {"", "a name", "a\nname", "a\x7f", "a\xff",
                            "123456789012345678901234567890123"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        rejected(names, one_name(names, invalid[i]));

    const char *duplicates[] = {"same", "same"};
    const size_t duplicate_sizes[] = {4, 4};
    rejected(names, name_list(names, duplicates, duplicate_sizes, 2));
    const char *many[] = {"a", "b", "c", "d", "e"};
    const size_t many_sizes[] = {1, 1, 1, 1, 1};
    rejected(names, name_list(names, many, many_sizes, 5));
    const char *embedded_nul[] = {"a\0suffix"};
    const size_t embedded_size[] = {8};
    rejected(names, name_list(names, embedded_nul, embedded_size, 1));
    rejected((const uint8_t[]){0x80}, 1); /* canonical empty grants omit extension */
    rejected((const uint8_t[]){0x01}, 1); /* not an array */
    rejected((const uint8_t[]){0x81, 0x61, 'a'}, 3); /* bytes, not text */
    rejected((const uint8_t[]){0x81, 0x42, 'a'}, 3); /* truncated name */
    rejected((const uint8_t[]){0x81, 0x41, 'a', 0}, 4); /* trailing field */
}

static void maximum_declaration(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    fixture_request request = fixture_node(1, 10);
    uint8_t names[256], wire[CORE_MAX_REQUEST_BYTES];
    const char *maximum[] = {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                             "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                             "cccccccccccccccccccccccccccccccc",
                             "dddddddddddddddddddddddddddddddd"};
    const size_t lengths[] = {32, 32, 32, 32};
    const size_t names_size = name_list(names, maximum, lengths, 4);
    const size_t size = native_request(&request, names, names_size, wire);
    const core_admission_record *record;
    CHECK(core_request_submit(&pool, 99, wire, size, 10, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->request.imports.count == 4);
    for (size_t i = 0; i < 4; ++i)
        CHECK(strcmp(record->request.imports.names[i], maximum[i]) == 0);
}

typedef struct {
    core_runtime_state states[CORE_MAX_NODE_RESERVATIONS];
    core_native_imports received;
    unsigned ordinary_calls, native_calls;
    bool reject;
} fake_runtime;

static bool prepare(void *context, size_t slot, uint8_t *artifact, size_t size,
                     const uint8_t *configuration, size_t configuration_size, size_t arena)
{
    fake_runtime *runtime = context;
    CHECK(artifact && size == 16 && arena == 112);
    CHECK(configuration_size == 3 && memcmp(configuration, "cfg", 3) == 0);
    runtime->states[slot] = (core_runtime_state){0};
    runtime->received = (core_native_imports){0};
    ++runtime->ordinary_calls;
    return !runtime->reject;
}

static bool prepare_native(void *context, size_t slot, uint8_t *artifact, size_t size,
                            const uint8_t *configuration, size_t configuration_size, size_t arena,
                            const core_native_imports *imports)
{
    fake_runtime *runtime = context;
    CHECK(artifact && size == 16 && arena == 112);
    CHECK(configuration_size == 3 && memcmp(configuration, "cfg", 3) == 0);
    CHECK(core_native_imports_valid(imports));
    runtime->states[slot] = (core_runtime_state){0};
    runtime->received = *imports;
    ++runtime->native_calls;
    return !runtime->reject;
}

static core_runtime_state *lock(void *context, size_t slot)
{
    return &((fake_runtime *)context)->states[slot];
}

static void unlock(void *context, size_t slot)
{
    (void)context;
    (void)slot;
}

static void dispatch_grants(void)
{
    /* Both a missing capability and an explicit adapter rejection must fail
     * without falling back to the ungranted entrypoint or leaking charges. */
    for (unsigned mode = 0; mode < 3; ++mode) {
        core_admission pool;
        fixture_pool(&pool, 1);
        fake_runtime runtime = {.reject = mode == 1};
        core_node_service service = {.admission = &pool, .runtime = {
            .context = &runtime, .prepare = prepare, .lock = lock, .unlock = unlock,
            .prepare_with_imports = mode == 0 ? NULL : prepare_native}};
        fixture_request request = fixture_node(1, 10);
        request.node.configuration_size = 3;
        memcpy(request.node.configuration, "cfg", 3);
        uint8_t names[256], wire[CORE_MAX_REQUEST_BYTES], artifact[16] = {0};
        const size_t names_size = one_name(names, "fixture_temperature_read");
        const size_t size = native_request(&request, names, names_size, wire);
        const core_admission_record *record;
        CHECK(core_request_submit(&pool, 99, wire, size, 10, &record) == CORE_ADMIT_ACCEPTED);
        CHECK(core_lifetime_charge(&record->node->lifetime, sizeof(artifact)));
        CHECK(core_node_artifact(&service, request.id, artifact, sizeof(artifact), 15) ==
            (mode == 2));
        CHECK(runtime.ordinary_calls == 0);
        CHECK(runtime.native_calls == (mode == 0 ? 0u : 1u));
        if (mode == 2) {
            CHECK(runtime.received.count == 1);
            CHECK(strcmp(runtime.received.names[0], "fixture_temperature_read") == 0);
            CHECK(record->node->lifetime.bytes == 128);
            runtime.states[0].prepare_failed = true;
            core_node_step(&service, 20);
            CHECK(runtime.states[0].cancel_requested && runtime.states[0].dispose_requested);
            runtime.states[0].disposed = true;
        } else {
            CHECK(record->cleanup_selected);
            CHECK(record->evidence.objects[0] & CORE_OBJECT_FAILED);
            CHECK(record->node->lifetime.bytes == sizeof(artifact));
            CHECK(core_lifetime_uncharge(&record->node->lifetime, sizeof(artifact)));
        }
        core_node_step(&service, 21);
        CHECK(pool.reserved_bytes == 0);
        CHECK(core_report_outcome(&record->evidence) == CORE_OUTCOME_FAILED_CLEAN);
        CHECK(core_admission_forget(&pool, request.id));

        /* Reusing that physical slot with an empty declaration dispatches the
         * ordinary entrypoint and supplies no previous node's grants. */
        request.id.value = 2;
        CHECK(fixture_submit(&pool, &request, &record) == CORE_ADMIT_ACCEPTED);
        runtime.reject = false;
        CHECK(record->request.imports.count == 0);
        CHECK(core_lifetime_charge(&record->node->lifetime, sizeof(artifact)));
        CHECK(core_node_artifact(&service, request.id, artifact, sizeof(artifact), 15));
        CHECK(runtime.ordinary_calls == 1 && runtime.received.count == 0);
        runtime.states[0].prepare_failed = true;
        core_node_step(&service, 20);
        runtime.states[0].disposed = true;
        core_node_step(&service, 21);
        CHECK(pool.reserved_bytes == 0);
    }
}

int main(void)
{
    accepted_declarations();
    malformed_declarations();
    maximum_declaration();
    dispatch_grants();
    puts("native request and dispatch checks passed");
    return 0;
}

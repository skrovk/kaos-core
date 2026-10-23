/* Exercise the production host port's deterministic service boundary without
 * sockets or wall-clock sleeps. These are new P2 tests, not a runtime oracle. */
#define main p2_host_program
#include "../p2_fixture_host.c"
#undef main
#include "admission_fixture.h"

static void late_removal(void)
{
    fixture_pool(&admission, 1);
    const fixture_request request = {.id = {20}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 10,
        .instance = {10}, .allowance_us = 1}};
    const core_admission_record *record;
    CHECK(fixture_submit(&admission, &request, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(record->deadline_us == 11);
    service(12);
    CHECK(record->evidence.history & CORE_HISTORY_CUTOFF);
    CHECK(core_report_outcome(&record->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_report_obligations(&record->evidence) == 0);
}

static void remove_one_binding(void)
{
    /* Separate empty-deployment fixture; no outstanding callbacks survive. */
    memset(cleanup_deadlines, 0, sizeof(cleanup_deadlines));
    fixture_pool(&admission, 1);
    fixture_request first = fixture_node(1, 10), second = fixture_node(2, 20);
    const core_admission_record *record;
    CHECK(fixture_submit(&admission, &first, &record) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_establish_node(&admission, (core_instance_id){1}));
    second.scope.kind = CORE_REQUEST_ADD_NODE;
    second.scope.channel_count = 4;
    const core_predicate_token always = {.kind = CORE_PREDICATE_TRUE};
    CHECK(core_predicate_init(&second.scope.predicate, &always, 1, 4));
    for (unsigned i = 0; i < 4; ++i) {
        second.scope.bindings[i].channel = fixture_channel(20, 10, 2, 1, 1, 1);
        second.scope.bindings[i].channel.service += i;
        second.scope.bindings[i].creating_op.value = 2;
    }
    CHECK(fixture_submit(&admission, &second, &record) == CORE_ADMIT_ACCEPTED);
    const size_t before = admission.reserved_bytes;
    fixture_request remove = {.id = {3}, .scope = {.kind = CORE_REQUEST_REMOVE_EDGE,
        .channel_count = 1, .allowance_us = 100}};
    remove.scope.bindings[0] = second.scope.bindings[1];
    CHECK(fixture_submit(&admission, &remove, &record) == CORE_ADMIT_ACCEPTED);
    select_cleanup(find(remove.id), 20, "scoped test removal");
    CHECK(core_report_outcome(&record->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(admission.reserved_bytes == before - 2 * CORE_ENDPOINT_BYTES);
    size_t remaining = 0;
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        if (!admission.endpoints[i].occupied) continue;
        ++remaining;
        CHECK(!admission.endpoints[i].lifetime.closing);
    }
    CHECK(remaining == 6);
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i)
        CHECK(admission.nodes[i].occupied && !admission.nodes[i].lifetime.closing);
}

int main(void)
{
    late_removal();
    remove_one_binding();
    puts("host service: late cutoff history and scoped binding cleanup passed");
    return 0;
}

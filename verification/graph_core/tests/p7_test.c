#include "admission_fixture.h"
#include "kaos_node.h"
#include "kaos_control.h"

static core_admission_record *mutable(core_admission *pool, const core_admission_record *r)
{
    return &pool->records[r - pool->records];
}

static fixture_request absent_removal(uint64_t id)
{
    return (fixture_request){.id = {id}, .scope = {.kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES,
        .logical_node_id = 90, .instance = {900000}, .allowance_us = 50}};
}

static const core_admission_record *finish(core_admission *pool, uint64_t id, bool failed)
{
    const fixture_request request = failed ? fixture_node(id, 10) : absent_removal(id);
    const core_admission_record *r;
    CHECK(fixture_submit(pool, &request, &r) == CORE_ADMIT_ACCEPTED);
    if (failed) CHECK(core_admission_close_scope(pool, r->id));
    core_node_service service = {.admission = pool};
    core_node_step(&service, 11);
    CHECK(core_report_outcome(&r->evidence) ==
        (failed ? CORE_OUTCOME_FAILED_CLEAN : CORE_OUTCOME_SUCCEEDED));
    CHECK(r->node == NULL && pool->reserved_bytes == 0);
    core_admission_maintain(pool);
    return r;
}

static void oldest_eligible_and_atomic_admission(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i)
        finish(&pool, 100 + i, i % 2 == 0);
    /* Final evidence survives actual execution/resource reclamation. */
    const core_admission_record *old = core_admission_find(&pool, (core_op_id){100});
    const uint8_t revision = old->evidence.revision;
    uint8_t bytes[CORE_REPORT_BYTES];
    size_t size;
    CHECK(core_report_encode(&old->evidence, bytes, sizeof(bytes), &size));
    CHECK(core_admission_report_retained(old));
    core_admission before = pool;
    fixture_request bad = fixture_node(9000, 20);
    bad.node.required_bytes = 4097;
    const core_admission_record *out = old;
    CHECK(fixture_submit(&pool, &bad, &out) == CORE_ADMIT_INVALID);
    CHECK(out == old && memcmp(&before, &pool, sizeof(pool)) == 0);
    CHECK(old->evidence.revision == revision);
    finish(&pool, 1000, false);
    CHECK(core_admission_find(&pool, (core_op_id){100}) == NULL);
    CHECK(core_admission_find(&pool, (core_op_id){101}) != NULL);
    /* ACKed younger history is reclaimed before older unacknowledged history. */
    const core_admission_record *young = core_admission_find(&pool, (core_op_id){1000});
    core_admission_acknowledge(&pool, young->id, 1, young->evidence.revision);
    const fixture_request fresh = absent_removal(1001);
    CHECK(fixture_submit(&pool, &fresh, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_find(&pool, (core_op_id){1000}) == NULL);
    CHECK(core_admission_find(&pool, (core_op_id){101}) != NULL);
    core_admission before_ack = pool;
    core_admission_acknowledge(&pool, (core_op_id){1000}, 1, young->evidence.revision);
    CHECK(memcmp(&before_ack, &pool, sizeof(pool)) == 0);
}

static void rejected_channel_preserves_reclaimable_history(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i)
        finish(&pool, 100 + i, false);
    const core_admission_record *old = core_admission_find(&pool, (core_op_id){100});
    const core_admission before = pool;
    const core_channel channel = fixture_channel(1000, 20, 1000, 20, 2, 1);
    uint8_t bytes[160];
    core_cbor_writer writer = {.data = bytes, .size = sizeof(bytes)};
    CHECK(core_cbor_write_array(&writer, 5));
    CHECK(core_cbor_write_uint(&writer, CORE_SCHEMA_VERSION));
    CHECK(core_cbor_write_uint(&writer, CORE_CHANNEL_COMMAND_KIND));
    CHECK(core_cbor_write_uint(&writer, 1000));
    CHECK(core_cbor_write_uint(&writer, 0));
    fixture_write_channel(&writer, &channel);
    const core_admission_record *out = old;
    /* Selection finds reclaimable history, then the absent local node rejects
     * preparation. Neither that history nor the caller's output is replaced. */
    CHECK(core_endpoint_channel_submit(&pool, 2, bytes, writer.offset, 10, false, &out) ==
          CORE_ADMIT_CONFLICT);
    CHECK(out == old && memcmp(&before, &pool, sizeof(pool)) == 0);
    /* The same command as cancellation needs no local node or endpoint. */
    CHECK(core_endpoint_channel_submit(&pool, 2, bytes, writer.offset, 10, true, &out) ==
          CORE_ADMIT_ACCEPTED);
    CHECK(core_admission_find(&pool, (core_op_id){100}) == NULL);
    CHECK(out->id.value == 1000 && out->channel_cleanup == 1 && pool.reserved_bytes == 0);
}

static void ack_never_discharges_unresolved(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        const fixture_request request = absent_removal(100 + i);
        const core_admission_record *r;
        CHECK(fixture_submit(&pool, &request, &r) == CORE_ADMIT_ACCEPTED);
        CHECK(core_report_record(&mutable(&pool, r)->evidence, CORE_HISTORY_CUTOFF) == CORE_REPORT_CHANGED);
        core_admission_acknowledge(&pool, r->id, 1, r->evidence.revision);
    }
    core_admission_maintain(&pool);
    core_admission before = pool;
    const fixture_request request = absent_removal(1000);
    const core_admission_record *out = NULL;
    CHECK(fixture_submit(&pool, &request, &out) == CORE_ADMIT_FULL);
    CHECK(out == NULL && memcmp(&before, &pool, sizeof(pool)) == 0);
    core_admission_record *r = &pool.records[0];
    const uint8_t old_revision = r->evidence.revision;
    const uint64_t deadline = r->deadline_us;
    const core_op_id id = r->id;
    core_node_service service = {.admission = &pool};
    core_node_step(&service, 100); /* Late attributed local absence closes all scopes. */
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(r->evidence.revision > old_revision && r->deadline_us == deadline);
    core_admission_acknowledge(&pool, id, 1, old_revision);
    CHECK(core_admission_report_retained(r));
    core_admission_maintain(&pool);
    CHECK(core_admission_find(&pool, id) == r);
    core_admission_acknowledge(&pool, id, 2, r->evidence.revision);
    CHECK(core_admission_report_retained(r));
    core_admission_acknowledge(&pool, id, 1, r->evidence.revision);
    core_admission_maintain(&pool);
    CHECK(core_admission_find(&pool, id) == NULL);
    CHECK(fixture_submit(&pool, &request, &out) == CORE_ADMIT_ACCEPTED);
}

static void changed_reports_age_without_clock_or_id_order(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) finish(&pool, 900 - i, false);
    core_admission_record *oldest = &pool.records[0];
    CHECK(core_report_observe(&oldest->evidence, 0,
        oldest->evidence.objects[0] | CORE_OBJECT_EXPIRED) == CORE_REPORT_CHANGED);
    core_admission_maintain(&pool);
    finish(&pool, 1000, false);
    CHECK(core_admission_find(&pool, (core_op_id){900}) != NULL);
    CHECK(core_admission_find(&pool, (core_op_id){899}) == NULL);
    /* Long churn never overflows the bounded age ranks. */
    for (uint64_t id = 1100; id < 1500; ++id) finish(&pool, id, id % 2 == 0);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i)
        CHECK(pool.records[i].report_age > 0 && pool.records[i].report_age <= CORE_MAX_ADMISSION_RECORDS);
}

static void exhausted_incarnation_refuses_reuse(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) pool.records[i].incarnation = UINT64_MAX;
    const fixture_request request = absent_removal(100);
    const core_admission_record *out = NULL;
    CHECK(fixture_submit(&pool, &request, &out) == CORE_ADMIT_FULL);
    CHECK(out == NULL);
}

static void forgotten_add_characterizes_q05(void)
{
    core_admission pool;
    fixture_pool(&pool, 1);
    const core_admission_record *old = finish(&pool, 100, true);
    const uint64_t incarnation = old->incarnation;
    const fixture_request replay = fixture_node(100, 10);
    const core_admission_record *out = NULL;
    CHECK(fixture_submit(&pool, &replay, &out) == CORE_ADMIT_DUPLICATE);
    CHECK(pool.reserved_bytes == 0);
    core_admission_acknowledge(&pool, old->id, 1, old->evidence.revision);
    core_admission_maintain(&pool);
    CHECK(core_admission_find(&pool, (core_op_id){100}) == NULL);
    CHECK(fixture_submit(&pool, &replay, &out) == CORE_ADMIT_ACCEPTED);
    CHECK(out->incarnation > incarnation && out->node->instance.value == 100);
    CHECK(pool.reserved_bytes == pool.node_bytes);
    puts("Q05 characterization: forgotten failed ADD admitted again and charged node capacity");
}

int main(void)
{
    oldest_eligible_and_atomic_admission();
    rejected_channel_preserves_reclaimable_history();
    ack_never_discharges_unresolved();
    changed_reports_age_without_clock_or_id_order();
    exhausted_incarnation_refuses_reuse();
    forgotten_add_characterizes_q05();
    puts("P7 retention: exact ACK, late closure, oldest eligible eviction, pressure and churn passed");
    return 0;
}

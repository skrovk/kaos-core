#include "kaos_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void roundtrip(const core_report *source)
{
    uint8_t data[CORE_REPORT_BYTES];
    size_t size = 0;
    core_report decoded;
    CHECK(core_report_encode(source, data, sizeof(data), &size));
    CHECK(size <= sizeof(data));
    CHECK(core_report_decode(data, size, source->kind, &decoded));
    CHECK(core_report_accept(&decoded, source) == CORE_REPORT_REPEAT);
    for (size_t i = 0; i < size; ++i)
        CHECK(!core_report_decode(data, i, source->kind, &decoded));
    CHECK(!core_report_encode(source, data, size - 1, &size));
}

int main(void)
{
    core_report r;
    CHECK(core_report_init(&r, 1, (core_op_id){UINT64_MAX}, UINT64_MAX, 1));
    CHECK(core_report_outcome(&r) == CORE_OUTCOME_PENDING);
    CHECK(core_report_observe(&r, 0, 0) == CORE_REPORT_REPEAT);
    CHECK(core_report_observe(&r, 0, CORE_OBJECT_ACTIVE) == CORE_REPORT_INVALID);
    CHECK(core_report_observe(&r, 0, CORE_OBJECT_STOPPED) == CORE_REPORT_INVALID);
    CHECK(core_report_record(&r, CORE_HISTORY_DISPATCH) == CORE_REPORT_INVALID);
    CHECK(core_report_observe(&r, 0, CORE_OBJECT_READY) == CORE_REPORT_CHANGED);
    CHECK(core_report_record(&r, CORE_HISTORY_COMMIT) == CORE_REPORT_CHANGED);
    CHECK(core_report_record(&r, CORE_HISTORY_DISPATCH) == CORE_REPORT_CHANGED);
    CHECK(core_report_record(&r, CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS) == CORE_REPORT_CHANGED);
    CHECK(core_report_outcome(&r) == CORE_OUTCOME_SUCCEEDED);
    core_report historical = r;
    CHECK(core_report_observe(&r, 0, CORE_OBJECT_READY | CORE_OBJECT_EXITED) == CORE_REPORT_CHANGED);
    CHECK(core_report_outcome(&r) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_report_accept(&r, &historical) == CORE_REPORT_OLD);
    roundtrip(&r);

    CHECK(core_report_init(&r, 2, (core_op_id){3}, 0, 9));
    CHECK(core_report_revision_bound(9) == 78);
    CHECK(core_report_record(&r, CORE_HISTORY_CLEANUP) == CORE_REPORT_CHANGED);
    CHECK(core_report_obligations(&r) == 511);
    CHECK(core_report_record(&r, CORE_HISTORY_CUTOFF) == CORE_REPORT_CHANGED);
    CHECK(core_report_outcome(&r) == CORE_OUTCOME_UNRESOLVED);
    const uint8_t closed = CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED;
    for (uint8_t i = 0; i < 9; ++i) {
        CHECK(core_report_observe(&r, i, closed) == CORE_REPORT_CHANGED);
        if (i < 8) CHECK(core_report_outcome(&r) == CORE_OUTCOME_UNRESOLVED);
    }
    CHECK(core_report_outcome(&r) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(core_report_obligations(&r) == 0);
    CHECK(core_report_record(&r, CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH |
        CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS) == CORE_REPORT_INVALID);
    roundtrip(&r);

    CHECK(core_report_init(&historical, 2, (core_op_id){3}, 0, 9));
    CHECK(core_report_accept(&historical, &r) == CORE_REPORT_CHANGED);
    CHECK(core_report_accept(&historical, &r) == CORE_REPORT_REPEAT);
    core_report conflict = r;
    conflict.history |= CORE_HISTORY_COMMIT;
    CHECK(core_report_accept(&historical, &conflict) == CORE_REPORT_CONFLICT);
    CHECK(core_report_accept(&historical, &r) == CORE_REPORT_REPEAT);
    conflict = r;
    conflict.coordinator = 1;
    CHECK(core_report_accept(&historical, &conflict) == CORE_REPORT_INVALID);
    core_owner_report owner = {.operation = {UINT64_MAX}, .owner = UINT64_MAX,
        .object = 8, .facts = CORE_OBJECT_READY};
    uint8_t owner_bytes[CORE_REPORT_BYTES];
    size_t owner_size;
    CHECK(core_owner_report_encode(&owner, owner_bytes, sizeof(owner_bytes), &owner_size));
    core_owner_report owner_copy;
    CHECK(core_owner_report_decode(owner_bytes, owner_size, &owner_copy));
    CHECK(owner_copy.operation.value == owner.operation.value && owner_copy.owner == owner.owner);
    CHECK(owner_copy.object == 8 && owner_copy.facts == CORE_OBJECT_READY);
    CHECK(!core_report_decode(owner_bytes, owner_size, 2, &conflict));
    for (size_t i = 0; i < owner_size; ++i)
        CHECK(!core_owner_report_decode(owner_bytes, i, &owner_copy));
    owner.facts = CORE_OBJECT_CLEANED;
    CHECK(!core_owner_report_encode(&owner, owner_bytes, sizeof(owner_bytes), &owner_size));
    conflict = r;
    conflict.revision += 1;
    conflict.history |= CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH |
        CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS;
    CHECK(core_report_accept(&historical, &conflict) == CORE_REPORT_INVALID);

    CHECK(core_report_init(&r, 6, (core_op_id){4}, 0, 2));
    CHECK(core_report_record(&r, CORE_HISTORY_CUTOFF) == CORE_REPORT_CHANGED);
    CHECK(core_report_outcome(&r) == CORE_OUTCOME_UNRESOLVED);
    CHECK(core_report_observe(&r, 0, closed) == CORE_REPORT_CHANGED);
    CHECK(core_report_observe(&r, 1, closed) == CORE_REPORT_CHANGED);
    CHECK(core_report_outcome(&r) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_report_record(&r, CORE_HISTORY_SUCCESS) == CORE_REPORT_INVALID);
    CHECK(core_report_observe(&r, 0, CORE_OBJECT_READY) == CORE_REPORT_CONFLICT);
    CHECK(core_report_observe(&r, 0, 0) == CORE_REPORT_OLD);
    CHECK(core_report_observe(&r, 0, CORE_OBJECT_STOPPED) == CORE_REPORT_INVALID);
    roundtrip(&r);

    /* Every allowed monotone fact can change once. Even the deliberately
     * maximal nine-object history fits the 8-bit aggregate stream. */
    CHECK(core_report_init(&r, 2, (core_op_id){5}, 0, 9));
    const uint8_t history[] = {1, 2, 4, 32, 8, 16};
    for (size_t i = 0; i < sizeof(history); ++i)
        CHECK(core_report_record(&r, history[i]) == CORE_REPORT_CHANGED);
    for (uint8_t i = 0; i < 9; ++i) {
        const uint8_t ordered[] = {1, 2, 4, 8, 16, 32, 64, 128};
        uint8_t facts = 0;
        for (uint8_t j = 0; j < 8; ++j) {
            facts |= ordered[j];
            CHECK(core_report_observe(&r, i, facts) == CORE_REPORT_CHANGED);
        }
    }
    CHECK(r.revision == 78);
    CHECK(core_report_record(&r, 63) == CORE_REPORT_REPEAT);
    CHECK(core_report_observe(&r, 0, 255) == CORE_REPORT_REPEAT);
    CHECK(core_report_observe(&r, 0, 127) == CORE_REPORT_OLD);
    CHECK(r.revision == 78);
    roundtrip(&r);
    CHECK(core_report_init(&historical, 1, (core_op_id){6}, 0, 1));
    conflict = historical;
    conflict.objects[0] = CORE_OBJECT_READY;
    conflict.revision = 14; /* one new fact cannot justify fourteen advances */
    CHECK(core_report_accept(&historical, &conflict) == CORE_REPORT_INVALID);
    puts("report: standalone snapshots, conflict isolation, late cleanup and revision bounds passed");
    return 0;
}

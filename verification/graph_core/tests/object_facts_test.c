#include "kaos_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void unchanged(core_report *r, uint8_t object, uint8_t facts,
                      core_report_result expected)
{
    const core_report before = *r;
    CHECK(core_report_observe(r, object, facts) == expected);
    CHECK(memcmp(&before, r, sizeof(before)) == 0);
}

static void reordered_owner_snapshots(void)
{
    core_report r;
    CHECK(core_report_init(&r, 3, (core_op_id){41}, 10, 2));
    unchanged(&r, 0, 0, CORE_REPORT_REPEAT);
    /* A complete snapshot can skip READY-only and add multiple facts. */
    CHECK(core_report_observe(&r, 0, 3) == CORE_REPORT_CHANGED);
    CHECK(r.revision == 1 && r.objects[0] == 3);
    unchanged(&r, 0, 3, CORE_REPORT_REPEAT);
    unchanged(&r, 0, 1, CORE_REPORT_OLD);
    unchanged(&r, 0, 0, CORE_REPORT_OLD);
    unchanged(&r, 0, 5, CORE_REPORT_CONFLICT); /* READY+FAILED loses ACTIVE */
    unchanged(&r, 0, 128, CORE_REPORT_CONFLICT); /* numeric magnitude is not ordering */
    unchanged(&r, 0, 32, CORE_REPORT_INVALID); /* CLEANED without STOPPED */
    unchanged(&r, 0, 16, CORE_REPORT_INVALID); /* STOPPED without CLEANUP */
    unchanged(&r, 0, 2, CORE_REPORT_INVALID);  /* ACTIVE without READY */
    unchanged(&r, 2, 1, CORE_REPORT_INVALID);  /* outside immutable scope */
    CHECK(core_report_observe(&r, 0, 59) == CORE_REPORT_CHANGED);
    CHECK(r.revision == 2);
    unchanged(&r, 0, 3, CORE_REPORT_OLD);
    CHECK(core_report_record(&r, CORE_HISTORY_CLEANUP) == CORE_REPORT_CHANGED);
    CHECK(core_report_obligations(&r) == 2);
    CHECK(core_report_record(&r, CORE_HISTORY_CUTOFF) == CORE_REPORT_CHANGED);
    CHECK(core_report_outcome(&r) == CORE_OUTCOME_UNRESOLVED);
    const core_report before_closure = r;
    CHECK(core_report_observe(&r, 1, 56) == CORE_REPORT_CHANGED);
    CHECK(core_report_outcome(&r) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(core_report_accept(&r, &before_closure) == CORE_REPORT_OLD);
    const core_report closed = r;
    core_report incoming = r;
    incoming.objects[0] = 120; /* adds EXITED while dropping READY and ACTIVE */
    ++incoming.revision;
    CHECK(core_report_accept(&r, &incoming) == CORE_REPORT_INVALID);
    CHECK(memcmp(&r, &closed, sizeof(r)) == 0);
    incoming = r;
    ++incoming.revision;
    CHECK(core_report_accept(&r, &incoming) == CORE_REPORT_INVALID);
    incoming = r;
    incoming.objects[1] |= CORE_OBJECT_EXPIRED;
    CHECK(core_report_accept(&r, &incoming) == CORE_REPORT_CONFLICT);
    CHECK(memcmp(&r, &closed, sizeof(r)) == 0);
}

static void schema_three_wire_shapes(void)
{
    uint8_t encoded[CORE_REPORT_BYTES];
    size_t size;
    core_report r, decoded;
    CHECK(core_report_init(&r, 3, (core_op_id){7}, 10, 2));
    CHECK(core_report_observe(&r, 0, 3) == CORE_REPORT_CHANGED);
    /* [3,8,7,10,1,Pending,0,[READY|ACTIVE,0]] */
    const uint8_t expected[] = {0x88, 4, 8, 7, 10, 1, 0, 0, 0x82, 3, 0};
    CHECK(core_report_encode(&r, encoded, sizeof(encoded), &size));
    CHECK(size == sizeof(expected) && memcmp(encoded, expected, size) == 0);
    CHECK(core_report_decode(expected, sizeof(expected), 3, &decoded));
    CHECK(core_report_accept(&decoded, &r) == CORE_REPORT_REPEAT);
    encoded[1] = 1;
    CHECK(!core_report_decode(encoded, size, 3, &decoded));
    /* Old pairs are invalid even if relabelled as schema 3. */
    const uint8_t pairs[] = {0x88, 4, 8, 7, 10, 1, 0, 0, 0x82, 0x82, 1, 3, 0x82, 0, 0};
    CHECK(!core_report_decode(pairs, sizeof(pairs), 3, &decoded));
    core_owner_report owner = {.operation = {7}, .owner = 10, .object = 1, .facts = 3};
    core_owner_report copy;
    const uint8_t owner_expected[] = {0x86, 4, 10, 7, 10, 1, 3};
    CHECK(core_owner_report_encode(&owner, encoded, sizeof(encoded), &size));
    CHECK(size == sizeof(owner_expected) && memcmp(encoded, owner_expected, size) == 0);
    CHECK(core_owner_report_decode(owner_expected, sizeof(owner_expected), &copy));
    CHECK(copy.operation.value == 7 && copy.owner == 10 && copy.object == 1 && copy.facts == 3);
    encoded[1] = 1;
    CHECK(!core_owner_report_decode(encoded, size, &copy));
    const uint8_t owner_revision[] = {0x87, 4, 10, 7, 10, 1, 1, 3};
    CHECK(!core_owner_report_decode(owner_revision, sizeof(owner_revision), &copy));
    owner.facts = 0;
    CHECK(core_owner_report_encode(&owner, encoded, sizeof(encoded), &size));
    CHECK(core_owner_report_decode(encoded, size, &copy) && copy.facts == 0);
}

int main(void)
{
    reordered_owner_snapshots();
    schema_three_wire_shapes();
    puts("object facts: containment, unchanged rejection, aggregate ACK identity and schema 3 passed");
    return 0;
}

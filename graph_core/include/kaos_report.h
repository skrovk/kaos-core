#ifndef KAOS_REPORT_H
#define KAOS_REPORT_H

#include "kaos_graph.h"

#define CORE_REPORT_MAX_OBJECTS 9
#define CORE_REPORT_BYTES 128
#define CORE_REPORT_KIND 8
#define CORE_QUERY_KIND 7
#define CORE_STORAGE_ACK_KIND 9
#define CORE_OWNER_REPORT_KIND 10

/* Monotone historical facts, not present permission or a substitute for the
 * operation engine's identity, issued-work, deadline and execution guards.
 * STOPPED: new work/access is blocked and all in-flight execution, callbacks
 * and tracked accesses have ended. Owned resources may remain allocated.
 * CLEANED additionally requires release of owned resources and associations.
 * These facts concern the indexed object, not the whole hosting device. */
enum {
    CORE_OBJECT_READY = 1, CORE_OBJECT_ACTIVE = 2, CORE_OBJECT_FAILED = 4,
    CORE_OBJECT_CLEANUP = 8, CORE_OBJECT_STOPPED = 16,
    CORE_OBJECT_CLEANED = 32, CORE_OBJECT_EXITED = 64, CORE_OBJECT_EXPIRED = 128
};
enum {
    CORE_HISTORY_COMMIT = 1, CORE_HISTORY_DISPATCH = 2,
    CORE_HISTORY_STARTED = 4, CORE_HISTORY_CLEANUP = 8,
    CORE_HISTORY_CUTOFF = 16, CORE_HISTORY_SUCCESS = 32
};
typedef enum {
    CORE_OUTCOME_PENDING, CORE_OUTCOME_SUCCEEDED,
    CORE_OUTCOME_FAILED_CLEAN, CORE_OUTCOME_UNRESOLVED
} core_outcome;
typedef struct { uint8_t revision; uint8_t facts; } core_object_report;
typedef struct {
    core_op_id operation;
    uint64_t owner;
    uint8_t object; /* index into the immutable authorized operation scope */
    core_object_report state;
} core_owner_report;
typedef struct {
    core_op_id id;
    uint64_t coordinator;
    uint8_t kind;
    uint8_t revision;
    uint8_t history;
    uint8_t object_count;
    core_object_report objects[CORE_REPORT_MAX_OBJECTS];
} core_report;
typedef enum {
    CORE_REPORT_CHANGED, CORE_REPORT_REPEAT, CORE_REPORT_OLD,
    CORE_REPORT_CONFLICT, CORE_REPORT_INVALID, CORE_REPORT_EXHAUSTED
} core_report_result;

/* Initial revision 0 plus at most eight new facts per object and six history
 * facts. Retries, queries, diagnostics, ACKs and unchanged snapshots consume
 * no revisions. Each owner emits only when adding at least one fact. */
uint8_t core_report_revision_bound(uint8_t object_count);
bool core_report_init(core_report *report, uint8_t kind, core_op_id id,
                      uint64_t coordinator, uint8_t object_count);
core_outcome core_report_outcome(const core_report *report);
uint16_t core_report_obligations(const core_report *report);
/* Call only after the engine validates the reporting owner and exact object
 * in the immutable scope. Object revisions compare only within that stream.
 * Invalid/conflicting evidence never changes retained facts or selects cleanup. */
core_report_result core_report_observe(core_report *report, uint8_t object,
                                       uint8_t revision, uint8_t facts);
core_report_result core_report_record(core_report *report, uint8_t history);
/* Complete cumulative snapshot; keys/owners are indexed by immutable request
 * scope: optional node, then source/destination of each channel. Does not
 * require earlier progress messages. No partial result or silent truncation. */
bool core_report_encode(const core_report *report, uint8_t *data, size_t size,
                        size_t *written);
bool core_report_decode(const uint8_t *data, size_t size, uint8_t request_kind,
                        core_report *out);
core_report_result core_report_accept(core_report *current, const core_report *incoming);
/* Distinct owner/object stream [1,10,opId,hostingKaOS,objectIndex,revision,facts].
 * Before observe(), the engine must match sender, operation, indexed object
 * owner and issued action. Decoding alone establishes none of those guards. */
bool core_owner_report_encode(const core_owner_report *report, uint8_t *data,
                              size_t size, size_t *written);
bool core_owner_report_decode(const uint8_t *data, size_t size, core_owner_report *out);

#endif

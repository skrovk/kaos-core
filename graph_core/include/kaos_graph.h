#ifndef KAOS_GRAPH_H
#define KAOS_GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* P1 foundations only: these records do not implement graph operations or
 * constitute STARTED/CLEANED evidence. The caller serializes all transitions
 * under one short local lock, including access admission versus closure.
 * Never hold that lock across worker execution, allocation, or waits. */

typedef struct { uint64_t value; } go_op_id;
typedef struct { uint64_t value; } go_instance_id;

/* O owns one sequence, initialized to zero, for the entire deployment.
 * IDs start at 1. UINT64_MAX is usable once; exhaustion never wraps. */
bool go_next_op_id(uint64_t *last, go_op_id *out);

/* All time values are owner-local monotonic microseconds. Pass the time at
 * the serialized decision boundary, not an event's arrival timestamp.
 * Tests inject time by passing explicit values; the core never reads a clock.
 * Construct each deadline once at its specified start event. */
bool go_duration_us(uint64_t milliseconds, uint64_t *out);
bool go_deadline(uint64_t now_us, uint64_t allowance_us, uint64_t *out);
bool go_before_deadline(uint64_t now_us, uint64_t deadline_us);
bool go_clock_now(uint64_t *out_us); /* supplied by the host or ESP-IDF port */

/* Provisional shared host/embedded bound, not a qualified device profile. */
#define GO_MAX_ACCESSES 4

typedef struct go_lifetime go_lifetime;
typedef struct {
    go_lifetime *owner;
    uint64_t ticket;
} go_access;

struct go_lifetime {
    size_t byte_limit;
    size_t bytes;
    size_t peak_bytes;
    size_t allocations;
    uint64_t last_ticket;
    uint64_t accesses[GO_MAX_ACCESSES];
    bool closing;
};

/* Caller-owned records must have stable addresses for the whole core run.
 * Initialize once, never copy/reset a used record: old queued tokens may still
 * refer to its address. Reopen preserves the nonwrapping ticket sequence.
 * Fields are readable for ledgers/observation; mutate only through this API. */
#define GO_LIFETIME_INIT(limit) { .byte_limit = (limit) }

/* Reserve one positive-sized allocation before allocating/dispatching work.
 * A failed allocation stays charged until its reservation is released.
 * This is an accounting ledger, not an allocator or admission transaction.
 * Close and drain accesses before freeing resources, then release each charge
 * exactly once. This conservative first slice does not free individual owned
 * resources while an owner is open. Associations/reservations outside this
 * ledger also gate CLEANED. All API pointers must be non-null. */
bool go_lifetime_charge(go_lifetime *owner, size_t bytes);
bool go_lifetime_uncharge(go_lifetime *owner, size_t bytes);
bool go_access_acquire(go_lifetime *owner, go_access *out);
bool go_access_release(go_lifetime *owner, go_access access);
void go_lifetime_close(go_lifetime *owner);
bool go_lifetime_quiescent(const go_lifetime *owner);
bool go_lifetime_reclaimed(const go_lifetime *owner);
bool go_lifetime_reopen(go_lifetime *owner);

typedef enum {
    GO_ACTION_EMPTY,
    GO_ACTION_ISSUED,
    GO_ACTION_EXECUTING,
    GO_ACTION_COMPLETED
} go_action_phase;

typedef enum {
    GO_WORK_SUCCEEDED,
    GO_WORK_FAILED,
    GO_WORK_CANCELLED
} go_work_result;

/* One caller-owned mailbox per outstanding action. Its access remains charged
 * through completion acceptance, so publishing completion needs no queue slot
 * or allocation. Wakeups are hints; the mailbox retains the result.
 * A worker carries a COPY of the issued token, never just an action pointer.
 * Only go_action_accept releases an action's access; do not release it directly.
 * The service keeps private allocations under the owner until disposal; this
 * mailbox intentionally carries no resource pointer or protocol evidence. */
typedef struct {
    go_access access;
    go_action_phase phase;
    go_work_result result;
} go_action;

bool go_action_issue(go_action *action, go_lifetime *owner, go_access *out);
/* False means do not execute. If closure won, publishes CANCELLED; stale or
 * repeated starts have no effect. A successful begin registers execution;
 * later closure requires cancellation/quiescence of that executing worker. */
bool go_action_begin(go_action *action, go_access token);
bool go_action_complete(go_action *action, go_access token, go_work_result result);
/* Acceptance releases the access, not the owner's resources. A successful
 * worker result does not authorize publication into a closing graph object. */
bool go_action_accept(go_action *action, go_access token, go_work_result *out);

#endif

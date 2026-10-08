#ifndef KAOS_GRAPH_H
#define KAOS_GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* P1 foundations only: these records do not implement graph operations or
 * constitute STARTED/CLEANED evidence. The caller serializes all transitions
 * under one short local lock, including access admission versus closure.
 * Never hold that lock across worker execution, allocation, or waits. */

/* Shared protocol identities received from O; K does not allocate them. */
typedef struct { uint64_t value; } core_op_id;
typedef struct { uint64_t value; } core_instance_id;

/* All time values are owner-local monotonic microseconds. Pass the time at
 * the serialized decision boundary, not an event's arrival timestamp.
 * Tests inject time by passing explicit values; the core never reads a clock.
 * Construct each deadline once at its specified start event. */
bool core_duration_us(uint64_t milliseconds, uint64_t *out);
bool core_deadline(uint64_t now_us, uint64_t allowance_us, uint64_t *out);
bool core_before_deadline(uint64_t now_us, uint64_t deadline_us);
bool core_clock_now(uint64_t *out_us); /* supplied by the host or ESP-IDF port */

/* Provisional shared host/embedded bound, not a qualified device profile. */
#define CORE_MAX_ACCESSES 4

typedef struct core_lifetime core_lifetime;
typedef struct {
    core_lifetime *owner;
    uint64_t ticket;
} core_access;

struct core_lifetime {
    size_t byte_limit;
    size_t bytes;
    size_t peak_bytes;
    size_t allocations;
    uint64_t last_ticket;
    uint64_t accesses[CORE_MAX_ACCESSES];
    bool closing;
};

/* Caller-owned records must have stable addresses for the whole core run.
 * Initialize once, never copy/reset a used record: old queued tokens may still
 * refer to its address. Reopen preserves the nonwrapping ticket sequence.
 * Fields are readable for ledgers/observation; mutate only through this API. */
#define CORE_LIFETIME_INIT(limit) { .byte_limit = (limit) }

/* Reserve one positive-sized allocation before allocating/dispatching work.
 * A failed allocation stays charged until its reservation is released.
 * This is an accounting ledger, not an allocator or admission transaction.
 * Close and drain accesses before freeing resources, then release each charge
 * exactly once. This conservative first slice does not free individual owned
 * resources while an owner is open. Associations/reservations outside this
 * ledger also gate CLEANED. All API pointers must be non-null. */
bool core_lifetime_charge(core_lifetime *owner, size_t bytes);
bool core_lifetime_uncharge(core_lifetime *owner, size_t bytes);
bool core_access_acquire(core_lifetime *owner, core_access *out);
bool core_access_release(core_lifetime *owner, core_access access);
void core_lifetime_close(core_lifetime *owner);
/* No outstanding access tickets; owned resources may still be charged. */
bool core_lifetime_unclaimed(const core_lifetime *owner);
bool core_lifetime_reclaimed(const core_lifetime *owner);
bool core_lifetime_reopen(core_lifetime *owner);

typedef enum {
    CORE_ACTION_EMPTY,
    CORE_ACTION_ISSUED,
    CORE_ACTION_EXECUTING,
    CORE_ACTION_COMPLETED
} core_action_phase;

typedef enum {
    CORE_WORK_SUCCEEDED,
    CORE_WORK_FAILED,
    CORE_WORK_CANCELLED
} core_work_result;

/* One caller-owned mailbox per outstanding action. Its access remains charged
 * through completion acceptance, so publishing completion needs no queue slot
 * or allocation. Wakeups are hints; the mailbox retains the result.
 * A worker carries a COPY of the issued token, never just an action pointer.
 * Only core_action_accept releases an action's access; do not release it directly.
 * The service keeps private allocations under the owner until disposal; this
 * mailbox intentionally carries no resource pointer or protocol evidence. */
typedef struct {
    core_access access;
    core_action_phase phase;
    core_work_result result;
} core_action;

bool core_action_issue(core_action *action, core_lifetime *owner, core_access *out);
/* False means do not execute. If closure won, publishes CANCELLED; stale or
 * repeated starts have no effect. A successful begin registers execution;
 * later closure requires cancellation and confirmed stopping of that
 * executing worker. */
bool core_action_begin(core_action *action, core_access token);
bool core_action_complete(core_action *action, core_access token, core_work_result result);
/* Acceptance releases the access, not the owner's resources. A successful
 * worker result does not authorize publication into a closing graph object. */
bool core_action_accept(core_action *action, core_access token, core_work_result *out);

#endif

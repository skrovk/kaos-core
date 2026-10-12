#ifndef KAOS_NODE_H
#define KAOS_NODE_H

#include "kaos_admission.h"
#include "kaos_runtime.h"

typedef struct {
    core_op_id id;
    core_access worker;
    size_t artifact_bytes;
    size_t arena_bytes;
    bool occupied;
    bool worker_accepted;
    bool artifact_charged;
    bool arena_charged;
} core_node_execution;

/* Initialize once with zeroed slots and configured admission/runtime fields.
 * Mutating calls run on the serialized control task. Runtime mailbox locks are
 * held only for short decisions, never allocation, WAMR execution or waits.
 * Stable node lifetime tickets protect reuse until disposal is accepted. */
typedef struct {
    core_admission *admission;
    core_node_runtime runtime;
    core_node_execution executions[CORE_MAX_NODE_RESERVATIONS];
} core_node_service;

bool core_node_supported(const core_admission_record *record);
/* Read-only worker check of accepted dependencies and current local lifetimes
 * at actual entry. Caller holds the registry lock through publishing runtime
 * execution. The control task must apply admitted removals before releasing
 * that lock; workers never run lifecycle passes or consume other mailboxes. */
bool core_node_start_allowed(const core_node_service *service, size_t slot, uint64_t now_us);
/* Validated artifact has already been charged and moved out of its transfer,
 * leaving no transfer access. Success transfers it to the runtime worker;
 * on false the caller still owns/frees it, including after dispatch failure.
 * The engine charges the remaining declared runtime arena before dispatch. */
bool core_node_artifact(core_node_service *service, core_op_id id,
                        uint8_t *artifact, size_t size, uint64_t now_us);
/* Artifact acquisition/handoff failed. Select normal node cleanup; the caller
 * still drains/releases any staging storage it owns. */
void core_node_artifact_failed(core_node_service *service, core_op_id id, uint64_t now_us);
/* Select cleanup for removals, lost dependencies and expired node deadlines;
 * request cancellation without consuming runtime completion/INIT evidence.
 * The host applies this before edge activation, whose deadline may be later.
 * Also included in step() for callers driving the node module on its own. */
void core_node_apply_cleanup(core_node_service *service, uint64_t now_us);
/* Recognizes admitted removals, external cleanup selection/artifact failures,
 * runtime completions and strict processing-time deadlines. Closed staging
 * not handed off yet remains the artifact adapter's cleanup responsibility. */
void core_node_step(core_node_service *service, uint64_t now_us);
/* Earliest live addition/cleanup deadline; UINT64_MAX when none remains.
 * A cutoff is recorded once; pending reclamation advances on completion wakeups. */
uint64_t core_node_next_deadline(const core_node_service *service);

#endif

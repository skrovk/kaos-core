#ifndef KAOS_HOST_H
#define KAOS_HOST_H

#include "kaos_artifact.h"
#include "kaos_control.h"
#include "kaos_host_platform.h"
#include "kaos_node.h"
#include <stdatomic.h>

/* Fixed per-round packet limits; no queue or task per traffic class. */
#ifndef CORE_HOST_APPLICATION_RX_BUDGET
#define CORE_HOST_APPLICATION_RX_BUDGET 4u
#endif
#ifndef CORE_HOST_MANAGEMENT_RX_BUDGET
#define CORE_HOST_MANAGEMENT_RX_BUDGET 1u
#endif
#ifndef CORE_HOST_APPLICATION_TX_BUDGET
#define CORE_HOST_APPLICATION_TX_BUDGET CORE_MAX_ENDPOINT_RESERVATIONS
#endif
#ifndef CORE_HOST_ARTIFACT_INTERVAL_US
#define CORE_HOST_ARTIFACT_INTERVAL_US UINT64_C(1000)
#endif
_Static_assert(CORE_HOST_APPLICATION_RX_BUDGET > 0 && CORE_HOST_APPLICATION_RX_BUDGET <= 64,
               "application receive budget must be 1..64");
_Static_assert(CORE_HOST_MANAGEMENT_RX_BUDGET > 0 && CORE_HOST_MANAGEMENT_RX_BUDGET <= 4,
               "management receive budget must be 1..4 (bounded reply storage)");
_Static_assert(CORE_HOST_APPLICATION_TX_BUDGET > 0 && CORE_HOST_APPLICATION_TX_BUDGET <= 64,
               "application transmit budget must be 1..64");

typedef struct {
    uint64_t local_device, authority, collector;
    uint64_t peers[CORE_MAX_PEERS]; /* includes local_device */
    size_t peer_count, node_bytes, byte_limit;
    bool collector_enabled;
    uint64_t artifact_interval_us; /* min send-attempt spacing; 0 disables pacing */
} core_host_config;

/* Saturating control-task counters, inspect after stopping or on that task.
 * Socket receipt/submission is not guest consumption or physical execution. */
typedef struct {
    uint64_t rounds, received[CORE_HOST_CLASSES], receive_drops[CORE_HOST_CLASSES];
    uint64_t invalid_packets[CORE_HOST_CLASSES], sent[CORE_HOST_CLASSES];
    uint64_t send_failures[CORE_HOST_CLASSES], endpoint_drops, artifact_requests;
    uint64_t max_round_us, max_management_gap_us, artifact_receive_max_us;
    uint64_t artifact_complete_max_us;
} core_host_stats;

typedef struct {
    core_artifact transfer;
    core_op_id id;
    core_lifetime *owner;
    uint8_t *bytes;
    size_t size;
    bool used, charged;
} core_host_staging;

/* Caller-owned fixed storage; zero initialize once at a stable address. Fields
 * are exposed for static allocation/inspection, not external mutation. Only
 * one real WAMR host may run in a process. Adapters own their OS resources and
 * must outlive destroy(), including all worker notifications. */
typedef struct {
    /* Stable object reservations and their control-task lifecycle owners. */
    core_admission admission;
    core_control control;
    core_node_service nodes;
    core_endpoint_service endpoints;
    core_edge_service edges;
    /* Protocol exchanges borrow graph evidence; they do not own objects. */
    core_control_peer peers[CORE_MAX_PEERS - 1];
    size_t peer_count;
    /* Host-owned buffers until runtime acceptance; at most one active download. */
    core_host_staging staging[CORE_MAX_NODE_RESERVATIONS];
    core_artifact_client artifact;
    int active_staging;
    core_coap_ids ids;
    /* Platform resources outlive the workers; statistics/scheduling are host-owned. */
    core_host_platform platform;
    core_host_stats stats;
    uint64_t artifact_interval_us, next_artifact_send_us, last_management_us;
    bool management_observed;
    atomic_bool stopping;
    bool initialized;
} core_host;

bool core_host_init(core_host *host, const core_host_config *config,
                    const core_host_platform *platform);
/* One bounded service round: lifecycle decisions, application receives,
 * management receives, application sends, replies/management, paced artifact,
 * maintenance. Each input and its lifecycle decisions share the
 * registry lock. Output and allocation/free run outside it. Workers only
 * publish runtime evidence and check entry permission; they never schedule
 * node/edge passes. No packet bytes are borrowed after return, no network
 * waits or guest execution occur here. Only the control task calls step/run.
 * next_deadline is absolute (0 requests another round without waiting).
 * run() uses this same interface. */
bool core_host_step(core_host *host, uint64_t *next_deadline);
bool core_host_run(core_host *host);
/* Thread-safe normal task call, not a signal/ISR handler. */
void core_host_request_stop(core_host *host);
/* Stops/joins workers before releasing staging. Process shutdown is not
 * protocol CLEANED evidence and never makes the context reusable. */
void core_host_destroy(core_host *host);

#endif

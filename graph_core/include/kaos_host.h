#ifndef KAOS_HOST_H
#define KAOS_HOST_H

#include "kaos_artifact.h"
#include "kaos_control.h"
#include "kaos_host_platform.h"
#include "kaos_node.h"
#include <stdatomic.h>

typedef struct {
    uint64_t local_device, authority, collector;
    uint64_t peers[CORE_MAX_PEERS]; /* includes local_device */
    size_t peer_count, node_bytes, byte_limit;
    bool collector_enabled;
} core_host_config;

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
    core_admission admission;
    core_control control;
    core_node_service nodes;
    core_endpoint_service endpoints;
    core_edge_service edges;
    core_control_peer peers[CORE_MAX_PEERS - 1];
    size_t peer_count;
    core_host_staging staging[CORE_MAX_NODE_RESERVATIONS];
    core_artifact_client artifact;
    int active_staging;
    core_coap_ids ids;
    core_host_platform platform;
    atomic_bool stopping;
    bool initialized;
} core_host;

bool core_host_init(core_host *host, const core_host_config *config,
                     const core_host_platform *platform);
/* One bounded control turn: advance existing lifecycles, incorporate optional
 * input, advance its consequences, reclaim staging, then deliver output and
 * select the next deadline. Input and its lifecycle decisions share the
 * registry lock. Output and allocation/free run outside it. Workers only
 * publish runtime evidence and check entry permission; they never schedule
 * node/edge passes. No packet bytes are borrowed after return, no network
 * waits or guest execution occur here. Only the control task calls step/run.
 * next_deadline is absolute, and run() uses this same interface. */
bool core_host_step(core_host *host, const core_host_packet *packet, uint64_t *next_deadline);
bool core_host_run(core_host *host);
/* Thread-safe normal task call, not a signal/ISR handler. */
void core_host_request_stop(core_host *host);
/* Stops/joins workers before releasing staging. Process shutdown is not
 * protocol CLEANED evidence and never makes the context reusable. */
void core_host_destroy(core_host *host);

#endif

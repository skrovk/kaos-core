#ifndef KAOS_ADMISSION_H
#define KAOS_ADMISSION_H

#include "kaos_graph.h"
#include "kaos_predicate.h"
#include "kaos_report.h"

/* Provisional common host/device profile. Admission and all inspection run
 * under the caller's short local lock. No allocation, dispatch or waits occur
 * here. A reservation never authorizes guest execution or endpoint activation.
 * Static record costs are additional to the variable-byte quota. */
#define CORE_MAX_NODE_RESERVATIONS 2
#define CORE_MAX_ENDPOINT_RESERVATIONS 8
#define CORE_MAX_ADMISSION_RECORDS 4
#define CORE_ADMISSION_REPORT_BYTES 128
/* Also bounds complete current incidence per concrete local node instance. */
#define CORE_MAX_REQUEST_CHANNELS 4
#define CORE_MAX_PORTS 4
#define CORE_MAX_PEERS 8
#define CORE_MAX_PAYLOAD_BYTES 256
#define CORE_MAX_QUEUE_MESSAGES 4
#define CORE_ENDPOINT_BYTES (CORE_MAX_PAYLOAD_BYTES * CORE_MAX_QUEUE_MESSAGES)
#define CORE_SCHEMA_VERSION 1
#define CORE_MAX_REQUEST_BYTES 1024
#define CORE_MAX_ARTIFACT_REF_BYTES 32
#define CORE_MAX_CONFIGURATION_BYTES 64
#define CORE_MAX_ARTIFACT_BYTES UINT32_C(65536)

typedef enum {
    CORE_REQUEST_ADD_NODE_NO_EDGES = 1,
    CORE_REQUEST_ADD_NODE,
    CORE_REQUEST_ADD_EDGE,
    CORE_REQUEST_REMOVE_NODE_NO_EDGES,
    CORE_REQUEST_REMOVE_NODE,
    CORE_REQUEST_REMOVE_EDGE
} core_request_kind;

typedef enum { CORE_PORT_INPUT, CORE_PORT_OUTPUT } core_port_direction;
typedef struct {
    uint16_t id;
    uint16_t type;
    uint16_t max_payload;
    uint8_t direction;
} core_port;

typedef struct {
    uint64_t source_logical;
    uint64_t destination_logical;
    uint64_t service;
    core_instance_id source_instance;
    core_instance_id destination_instance;
    uint64_t source_owner;
    uint64_t destination_owner;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t type;
    uint16_t payload_bytes;
    uint8_t queue_messages;
    uint64_t addition_allowance_us;
} core_channel;

typedef struct {
    core_channel channel;
    core_op_id creating_op;
} core_binding;

typedef struct {
    uint64_t logical_node_id;
    uint64_t addition_allowance_us;
    uint64_t cleanup_allowance_us;
    uint32_t artifact_bytes;
    uint32_t artifact_crc32;
    uint32_t required_bytes;
    uint8_t artifact_ref_size;
    uint8_t configuration_size;
    uint8_t artifact_ref[CORE_MAX_ARTIFACT_REF_BYTES];
    uint8_t configuration[CORE_MAX_CONFIGURATION_BYTES];
} core_node_request;

/* Exact immutable scope, ordered node (when present), then source/destination
 * endpoint of each listed binding. Creation IDs of additions equal opId.
 * Ports/dynamic apply only to node additions; predicate to ADD_NODE.
 * Removal allowance is in allowance_us; addition cleanup has its own field.
 * Remote descriptors are O-authorized: each remote owner still independently
 * validates its own registry/ports and reserves before local preparation. */
typedef struct {
    uint8_t kind;
    uint8_t channel_count;
    uint8_t port_count;
    uint8_t object_count;
    bool dynamic_ports;
    uint64_t logical_node_id;
    uint64_t coordinator;
    core_instance_id instance;
    uint64_t allowance_us;
    uint64_t cleanup_allowance_us;
    core_port ports[CORE_MAX_PORTS];
    core_binding bindings[CORE_MAX_REQUEST_CHANNELS];
    core_predicate predicate;
} core_request_scope;

typedef struct {
    core_instance_id instance;
    core_lifetime lifetime;
    core_action prepare;
    uint64_t logical_node_id;
    core_port ports[CORE_MAX_PORTS];
    uint8_t port_count;
    bool dynamic_ports;
    bool established;
    bool occupied;
} core_reserved_node;

typedef struct {
    core_binding binding;
    core_lifetime lifetime;
    core_action prepare;
    bool sender;
    bool occupied;
} core_reserved_endpoint;

typedef struct {
    core_op_id id;
    uint64_t deadline_us;
    core_reserved_node *node;
    uint8_t report[CORE_ADMISSION_REPORT_BYTES];
    core_report evidence;
    core_node_request request;
    core_request_scope scope;
    bool request_present;
    bool cleanup_selected; /* retained local rejection fence, including pre-prepare cancel */
    bool endpoint_only; /* Ki reservation; deadline_us is not a Kc expiry */
    bool occupied;
} core_admission_record;

typedef struct {
    size_t node_bytes;
    size_t byte_limit;
    size_t reserved_bytes;
    size_t peak_reserved_bytes;
    uint64_t local_device;
    uint64_t authority;
    uint64_t peers[CORE_MAX_PEERS];
    uint8_t peer_count;
    bool configured;
    core_reserved_node nodes[CORE_MAX_NODE_RESERVATIONS];
    core_reserved_endpoint endpoints[CORE_MAX_ENDPOINT_RESERVATIONS];
    core_admission_record records[CORE_MAX_ADMISSION_RECORDS];
} core_admission;

typedef enum {
    CORE_ADMIT_ACCEPTED,
    CORE_ADMIT_DUPLICATE,
    CORE_ADMIT_INVALID,
    CORE_ADMIT_FULL,
    CORE_ADMIT_UNAUTHORIZED,
    CORE_ADMIT_CONFLICT
} core_admit_result;

/* Initialize once at a stable address. Never copy/reset a used pool: lifetime
 * tickets survive slot reuse. Failure leaves the pool unchanged. */
bool core_admission_init(core_admission *pool, size_t node_bytes, size_t byte_limit);
/* Configure enrolled owner IDs and O once before protocol submission. peers
 * contains the local device and every permitted remote owner (not necessarily
 * O); no discovery or cryptographic authentication is implied. */
bool core_admission_configure(core_admission *pool, uint64_t local_device,
                              uint64_t authority, const uint64_t *peers,
                              size_t peer_count);

/* Original resource-only prerequisite. Caller validates authority and empty
 * incidence. Retained duplicate precedes allowance checks. No request, report
 * outcome or managed work is produced. Kept for low-level reservation tests. */
core_admit_result core_node_admit(core_admission *pool, core_op_id id,
                                  uint64_t now_us, uint64_t allowance_us,
                                  const core_admission_record **out);

/* Strict deterministic CBOR [1,kind,opId,body:bstr], exact array shapes documented
 * in docs/graph_p2_contract.md. Bound/framing checks precede retained-ID discard;
 * duplicate bodies are never decoded/compared. Fresh requests validate every
 * local scope, capability and resource before one publication boundary. Sender
 * is derived from configured transport peer attribution, not a payload field.
 * All rejection paths preserve pool and out. Input may be freed on return.
 * This reserves an operation; P3-P6 own execution and outcome transitions. */
core_admit_result core_request_submit(core_admission *pool, uint64_t sender,
                                      const uint8_t *data, size_t size,
                                      uint64_t now_us,
                                      const core_admission_record **out);
/* Issued endpoint work carries the complete original O-authorized request.
 * A configured coordinator forwards it: node owner for node operations,
 * source owner for edges. Reserve every local endpoint role together; the
 * remote new node is not reserved here. Remove variants also install fences
 * before a delayed creator arrives. This is admission of issued work, not a
 * top-level resubmission filter. Later phases use the retained exact scope.
 * Ki does not interpret Kc's allowance as a local expiration: deadline_us=0. */
core_admit_result core_endpoint_submit(core_admission *pool, uint64_t coordinator,
                                       const uint8_t *data, size_t size,
                                       uint64_t now_us,
                                       const core_admission_record **out);
/* Original coordinator may cancel issued ADD scope without a new O operation.
 * Install its fence/report capacity even if PREPARE has not arrived. Existing
 * resources are closed, never freed before all relevant work and accesses
 * have stopped. Subsequent phase work
 * must observe cleanup_selected. Repeating cancellation resets no deadline. */
core_admit_result core_endpoint_cancel(core_admission *pool, uint64_t coordinator,
                                       const uint8_t *data, size_t size,
                                       uint64_t now_us,
                                       const core_admission_record **out);
/* Trusted local wrapper for initial no-edge profile; caller supplies authority
 * and incidence checks as in core_node_admit. Uses the finalized v1 shape. */
core_admit_result core_node_submit(core_admission *pool, const uint8_t *data,
                                   size_t size, uint64_t now_us,
                                   const core_admission_record **out);

const core_admission_record *core_admission_find(const core_admission *pool,
                                                core_op_id id);
/* P3's integration seam: call only after accepted startup success. It does not
 * fabricate startup evidence. Pending/removing/exited instances cannot receive
 * a newly added edge. */
bool core_admission_establish_node(core_admission *pool, core_instance_id instance);
/* P2 failure/removal handoff: close exactly local resources in this immutable
 * scope, suppressing subsequent access. Workers still must stop or complete;
 * these functions neither free allocations nor assert protocol CLEANED. */
bool core_admission_close_scope(core_admission *pool, core_op_id id);
bool core_admission_release_endpoints(core_admission *pool, core_op_id creating_id);
/* Release one exact binding while other channels from the same creation live. */
bool core_admission_release_binding(core_admission *pool, const core_binding *binding);
bool core_admission_release_node(core_admission *pool, core_op_id creating_id);
/* Caller establishes Q06 report retirement and remote-obligation eligibility.
 * Local owned resources must be released. Removing a record also retires its
 * retained rejection fence, subject to the explicit Q05 replay limitation. */
bool core_admission_forget(core_admission *pool, core_op_id id);

#endif

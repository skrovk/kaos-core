#ifndef KAOS_EDGE_H
#define KAOS_EDGE_H

#include "kaos_endpoint.h"

#define CORE_ACTIVATE_KIND 11
/* One 13-field channel plus creation, target and envelope fits even when
 * every identity and allowance uses the largest CBOR integer representation. */
#define CORE_EDGE_MESSAGE_BYTES 160
typedef enum {
    CORE_EDGE_PREPARE,
    CORE_EDGE_ACTIVATE,
    CORE_EDGE_CANCEL,
    CORE_EDGE_OWNER
} core_edge_effect_kind;

/* An owned message copy. The port starts one bounded CoAP exchange from it;
 * it must not manufacture another exchange when this one expires. */
typedef struct {
    core_edge_effect_kind kind;
    core_op_id operation;
    uint64_t incarnation;
    uint64_t peer, deadline_us;
    uint8_t object;
    uint8_t record_slot; /* together with incarnation, identifies local lifetime */
    uint8_t body[CORE_EDGE_MESSAGE_BYTES];
    size_t size;
} core_edge_effect;

typedef struct {
    core_op_id id;
    uint64_t incarnation;
    uint64_t channel_cleanup_deadline_us[CORE_MAX_REQUEST_CHANNELS];
    uint32_t prepare_issued, activate_issued, cleanup_issued;
    core_channel_set channel_cleanup_started, channel_cutoff;
    /* Ki: last emitted facts; Kc: accepted owner facts before adding local
     * cleanup selection/expiry to the aggregate. */
    uint8_t emitted[CORE_REPORT_MAX_OBJECTS];
    bool occupied;
} core_edge_execution;

typedef struct {
    core_admission *admission;
    core_endpoint_service *endpoints;
    core_edge_execution executions[CORE_MAX_ADMISSION_RECORDS];
    size_t next_record;
} core_edge_service;

/* Zero initialize with stable admission/endpoints. Caller serializes these
 * calls and endpoint data calls with the same short registry lock. */
bool core_edge_supported(const core_admission_record *record);
/* Complete, currently eligible channels; fact=0 yields PossibleSet. */
core_channel_set core_edge_channels(const core_admission_record *record, uint8_t fact);
void core_edge_step(core_edge_service *service, uint64_t now_us);
uint64_t core_edge_next_deadline(const core_edge_service *service);
/* Call only when this peer's transport slot is free. Success records issue
 * before bytes leave the process. Local work is driven by step(), not I/O. */
bool core_edge_next(core_edge_service *service, uint64_t peer, uint64_t now_us,
                    core_edge_effect *effect);
const char *core_edge_path(core_edge_effect_kind kind);
/* Before every retransmission, cancel obsolete constructive work. Owner
 * reports remain deliverable after coordinator cutoff. */
bool core_edge_effect_current(const core_edge_service *service, const core_edge_effect *effect,
                              uint64_t now_us);
/* Receipt success is not lifecycle evidence. A definitive command rejection
 * selects cleanup; network silence is resolved by the original deadline. */
void core_edge_rejected(core_edge_service *service, const core_edge_effect *effect,
                        uint64_t now_us);
/* A correlated successful /owner response permits retirement of that exact
 * final local closure copy. An empty transport ACK is not acceptance. */
void core_edge_delivered(core_edge_service *service, const core_edge_effect *effect);
bool core_edge_activate(core_edge_service *service, uint64_t sender, const uint8_t *body,
                        size_t size);
bool core_edge_owner(core_edge_service *service, uint64_t sender, const uint8_t *body, size_t size,
                     uint64_t now_us);
/* Correlated cleanup reply evidence may arrive after the exchange cutoff.
 * Match the original local record lifetime without requiring a live timer. */
bool core_edge_reply(core_edge_service *service, const core_edge_effect *effect, uint64_t sender,
                     const uint8_t *body, size_t size, uint64_t now_us);

#endif

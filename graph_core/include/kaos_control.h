#ifndef KAOS_CONTROL_H
#define KAOS_CONTROL_H

#include "kaos_admission.h"
#include "kaos_coap.h"
#include "kaos_edge.h"

/* NSTART=1: one peer cannot advance to its next exchange while retrying a
 * lost reply. Keep its latest accepted final owner response independently of
 * operation/report retirement. Enrolled ports must use strictly increasing
 * MIDs and refuse wrap (as both supplied ports do); generic CoAP alone does
 * not establish that order. No retained datagram or duplicate evidence. */
typedef struct {
    core_op_id operation;
    uint64_t deadline_us;
    uint8_t token[8];
    uint16_t message_id;
    uint8_t token_size, object, facts;
    bool occupied;
} core_owner_receipt;

/* One static control adapter per KaOS. The caller supplies peer attribution,
 * serialization, datagram I/O and monotonic time. Work stays in the admission
 * pool; this adapter does no network wait, allocation or guest execution. */
typedef struct {
    core_admission *admission;
    core_edge_service *edges; /* optional until the port enables P4 */
    uint64_t collector; /* designated storage recipient, configured before use */
    bool collector_enabled;
    core_owner_receipt owner_receipts[CORE_MAX_PEERS];
    core_coap_cache cache;
    core_coap_block1 assembly;
    uint64_t assembly_deadline_us;
    uint8_t assembly_resource;
} core_control;

/* Zero initialize with .admission pointing to a configured stable pool.
 * POST /graph accepts a typed operation; POST /prepare reserves remote
 * endpoints under the coordinator; POST /cancel selects original-addition
 * cleanup at an owner; POST /query takes [version,7,opId,h''].
 * O or the designated collector may query and POST /stored with
 * [version,9,opId,coordinator,revision]. This storage ACK never gates completion.
 * Missing history returns 4.04 with [version,14,opId,localDevice,observations],
 * where observations is [] or [revision,history,[facts...]] from continuing
 * operational state. These observations are not a recovered result/outcome.
 * With edges set, /activate accepts an attributed role command and /owner
 * accepts a cumulative owner report. These idempotent resources skip the
 * ordinary response cache and do not admit multipart requests.
 * P4 edge-cleanup replies include the remote receiver's cumulative owner
 * report. Unsegmented /prepare kind6 also bypasses the response cache so a
 * duplicate can return later CLEANED evidence without a new retry wave.
 * All bodies are application/cbor (60). Multipart submission requires Size1
 * and a nonempty Request-Tag; one inbound bulk body is retained at a time.
 * /cancel is unsegmented (every supported body fits one datagram), allowing
 * idempotent cleanup replies without consuming the ordinary response cache.
 * Reply bytes borrow no input. false means malformed datagram/no response.
 * A small output buffer is rejected BEFORE any admission mutation. */
bool core_control_receive(core_control *control, uint64_t sender,
                           const uint8_t *data, size_t size, uint64_t now_us,
                           uint8_t *response, size_t capacity, size_t *written);

#endif

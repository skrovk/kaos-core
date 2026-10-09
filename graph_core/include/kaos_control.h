#ifndef KAOS_CONTROL_H
#define KAOS_CONTROL_H

#include "kaos_admission.h"
#include "kaos_coap.h"

/* One static control adapter per KaOS. The caller supplies peer attribution,
 * serialization, datagram I/O and monotonic time. Work stays in the admission
 * pool; this adapter does no network wait, allocation or guest execution. */
typedef struct {
    core_admission *admission;
    core_coap_cache cache;
    core_coap_block1 assembly;
    uint64_t assembly_deadline_us;
    uint8_t assembly_resource;
} core_control;

/* Zero initialize with .admission pointing to a configured stable pool.
 * POST /graph accepts a typed operation; POST /prepare reserves remote
 * endpoints under the coordinator; POST /cancel selects original-addition
 * cleanup at an owner; POST /query takes [3,7,opId,h''].
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

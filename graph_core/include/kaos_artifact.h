#ifndef KAOS_ARTIFACT_H
#define KAOS_ARTIFACT_H

#include "kaos_graph.h"

#define CORE_ARTIFACT_BLOCK_BYTES 256
#define CORE_ARTIFACT_MAX_BYTES UINT32_C(65536)

typedef enum {
    CORE_ARTIFACT_EMPTY, CORE_ARTIFACT_RECEIVING, CORE_ARTIFACT_READY,
    CORE_ARTIFACT_FAILED, CORE_ARTIFACT_CANCELLED, CORE_ARTIFACT_MOVED
} core_artifact_state;

typedef struct {
    core_access access;
    uint8_t *storage;
    size_t expected_size, received_size;
    uint32_t expected_crc32;
    uint64_t deadline_us, token, last_token;
    uint8_t etag[8], etag_size, szx;
    core_artifact_state state;
    bool token_used;
} core_artifact;

typedef enum {
    CORE_ARTIFACT_IGNORED, CORE_ARTIFACT_MORE,
    CORE_ARTIFACT_COMPLETE, CORE_ARTIFACT_INVALID
} core_artifact_result;

/* CRC-32/ISO-HDLC: reflected polynomial 0xedb88320, init/xorout ffffffff,
 * refin/refout true. The standard ASCII "123456789" check is cbf43926. */
uint32_t core_artifact_crc32(const uint8_t *data, size_t size);

/* Zero initialize once at a stable address. Call only AFTER admission and
 * charging/allocating staging under owner. The caller retains allocation
 * ownership; this record exclusively owns write access until take/cancel.
 * All calls/receives are serialized: no worker may retain storage or views
 * obtained from this transfer after its synchronous call returns.
 * Staging capacity includes expected_size; owner keeps its allocation charged
 * through runtime use. The transfer never allocates, frees or uncharges it.
 * token identifies the attempt and is allocated monotonically without reuse.
 * The adapter correlates each CoAP exchange token with this attempt; distinct
 * tokens per block also isolate delayed separate responses from older GETs.
 * Deadline is the original operation deadline; no call resets it. */
bool core_artifact_begin(core_artifact *artifact, core_lifetime *owner,
                          uint8_t *storage, size_t capacity,
                          size_t expected_size, uint32_t expected_crc32,
                          uint64_t token, uint64_t now_us, uint64_t deadline_us);
/* Initial Block2 request is NUM=0,SZX=4. After an initial downsize, use the
 * returned block value unchanged for subsequent sequential GET requests. */
bool core_artifact_next(const core_artifact *artifact, uint64_t now_us,
                         uint32_t *block, uint64_t *token);
/* Caller first checks CoAP response/peer correlation and Content-Format.
 * Require ETag on every block, exact sequential offsets and complete length.
 * Old tokens and already-consumed identical blocks have no effects. CRC reads
 * the fully staged bytes, not just the received chunks. READY is only artifact
 * validity, never node READY/STARTED or an operation outcome. */
core_artifact_result core_artifact_receive(core_artifact *artifact,
                                           uint64_t token, uint32_t block,
                                           const uint8_t *etag, size_t etag_size,
                                           const uint8_t *payload,
                                           size_t payload_size, uint64_t now_us);
/* Permitted from RECEIVING/FAILED, before the original deadline. Reuses the
 * allocation after synchronous old access closure, discards progress/ETag,
 * and demands a fresh larger token. No new allocation or deadline. */
bool core_artifact_restart(core_artifact *artifact, uint64_t token, uint64_t now_us);
/* Explicit move to node/loading owner. Releases transfer access and clears its
 * storage pointer; the allocation remains charged to the same lifetime. */
bool core_artifact_take(core_artifact *artifact, uint64_t now_us,
                         uint8_t **storage, size_t *size);
/* Stops writes and releases transfer access. Caller closes/drains ALL owner
 * accesses before freeing and uncharging staging or asserting closure. */
void core_artifact_cancel(core_artifact *artifact);

#endif

#include "kaos_artifact.h"

#include <string.h>

uint32_t core_artifact_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) != 0 ? UINT32_C(0xedb88320) : 0);
        }
    }
    return crc ^ UINT32_MAX;
}

static bool live(const core_artifact *artifact)
{
    if (artifact->access.owner == NULL || artifact->access.owner->closing) {
        return false;
    }
    const core_lifetime *owner = artifact->access.owner;
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        if (owner->accesses[i] == artifact->access.ticket) {
            return true;
        }
    }
    return false;
}

bool core_artifact_begin(core_artifact *artifact, core_lifetime *owner,
                          uint8_t *storage, size_t capacity,
                          size_t expected_size, uint32_t expected_crc32,
                          uint64_t token, uint64_t now_us, uint64_t deadline_us)
{
    if (artifact->access.owner != NULL || storage == NULL || expected_size == 0 ||
        expected_size > CORE_ARTIFACT_MAX_BYTES || expected_size > capacity ||
        owner->bytes < expected_size || !core_before_deadline(now_us, deadline_us) ||
        (artifact->token_used && token <= artifact->last_token)) {
        return false;
    }
    core_access access;
    if (!core_access_acquire(owner, &access)) {
        return false;
    }
    *artifact = (core_artifact){
        .access = access, .storage = storage, .expected_size = expected_size,
        .expected_crc32 = expected_crc32, .deadline_us = deadline_us,
        .token = token, .last_token = token, .szx = 4,
        .state = CORE_ARTIFACT_RECEIVING, .token_used = true
    };
    return true;
}

bool core_artifact_next(const core_artifact *artifact, uint64_t now_us,
                         uint32_t *block, uint64_t *token)
{
    if (artifact->state != CORE_ARTIFACT_RECEIVING || !live(artifact) ||
        !core_before_deadline(now_us, artifact->deadline_us)) {
        return false;
    }
    const size_t block_size = (size_t)1 << (artifact->szx + 4);
    *block = (uint32_t)((artifact->received_size / block_size) << 4) | artifact->szx;
    *token = artifact->token;
    return true;
}

static core_artifact_result invalid(core_artifact *artifact)
{
    artifact->state = CORE_ARTIFACT_FAILED;
    return CORE_ARTIFACT_INVALID;
}

core_artifact_result core_artifact_receive(core_artifact *artifact,
                                           uint64_t token, uint32_t block,
                                           const uint8_t *etag, size_t etag_size,
                                           const uint8_t *payload,
                                           size_t payload_size, uint64_t now_us)
{
    if (artifact->state != CORE_ARTIFACT_RECEIVING || token != artifact->token ||
        !live(artifact)) {
        return CORE_ARTIFACT_IGNORED;
    }
    if (!core_before_deadline(now_us, artifact->deadline_us)) {
        return invalid(artifact);
    }
    const uint8_t szx = block & 7;
    const bool more = (block & 8) != 0;
    if (block > UINT32_C(0xffffff) || szx > artifact->szx || etag == NULL ||
        etag_size == 0 || etag_size > 8 || payload == NULL ||
        (artifact->received_size != 0 && szx != artifact->szx)) {
        return invalid(artifact);
    }
    const size_t block_size = (size_t)1 << (szx + 4);
    const size_t offset = (size_t)(block >> 4) * block_size;
    if (payload_size == 0 || payload_size > block_size ||
        (more && payload_size != block_size) || offset > artifact->expected_size ||
        payload_size > artifact->expected_size - offset) {
        return invalid(artifact);
    }
    const size_t end = offset + payload_size;
    if ((more && end >= artifact->expected_size) || (!more && end != artifact->expected_size)) {
        return invalid(artifact);
    }
    if (artifact->etag_size != 0 &&
        (artifact->etag_size != etag_size || memcmp(artifact->etag, etag, etag_size) != 0)) {
        return invalid(artifact);
    }
    if (offset < artifact->received_size) {
        if (end <= artifact->received_size &&
            memcmp(artifact->storage + offset, payload, payload_size) == 0) {
            return CORE_ARTIFACT_IGNORED;
        }
        return invalid(artifact);
    }
    if (offset != artifact->received_size) {
        return invalid(artifact);
    }
    if (artifact->etag_size == 0) {
        artifact->etag_size = (uint8_t)etag_size;
        memcpy(artifact->etag, etag, etag_size);
        artifact->szx = szx;
    }
    memcpy(artifact->storage + offset, payload, payload_size);
    artifact->received_size = end;
    if (more) {
        return CORE_ARTIFACT_MORE;
    }
    if (core_artifact_crc32(artifact->storage, artifact->expected_size) != artifact->expected_crc32) {
        return invalid(artifact);
    }
    artifact->state = CORE_ARTIFACT_READY;
    return CORE_ARTIFACT_COMPLETE;
}

bool core_artifact_restart(core_artifact *artifact, uint64_t token, uint64_t now_us)
{
    if ((artifact->state != CORE_ARTIFACT_RECEIVING && artifact->state != CORE_ARTIFACT_FAILED) ||
        !live(artifact) || token <= artifact->last_token ||
        !core_before_deadline(now_us, artifact->deadline_us)) {
        return false;
    }
    core_lifetime *owner = artifact->access.owner;
    if (!core_access_release(owner, artifact->access)) {
        return false;
    }
    artifact->access = (core_access){0};
    if (!core_access_acquire(owner, &artifact->access)) {
        artifact->state = CORE_ARTIFACT_FAILED;
        return false;
    }
    artifact->token = token;
    artifact->last_token = token;
    artifact->received_size = 0;
    artifact->etag_size = 0;
    artifact->szx = 4;
    artifact->state = CORE_ARTIFACT_RECEIVING;
    return true;
}

bool core_artifact_take(core_artifact *artifact, uint64_t now_us,
                         uint8_t **storage, size_t *size)
{
    if (artifact->state != CORE_ARTIFACT_READY || !live(artifact) ||
        !core_before_deadline(now_us, artifact->deadline_us) ||
        !core_access_release(artifact->access.owner, artifact->access)) {
        return false;
    }
    *storage = artifact->storage;
    *size = artifact->expected_size;
    artifact->access = (core_access){0};
    artifact->storage = NULL;
    artifact->state = CORE_ARTIFACT_MOVED;
    return true;
}

void core_artifact_cancel(core_artifact *artifact)
{
    if (artifact->access.owner != NULL) {
        if (!core_access_release(artifact->access.owner, artifact->access)) {
            return; /* A failed release never claims that accesses have stopped. */
        }
        artifact->access = (core_access){0};
    }
    artifact->storage = NULL;
    if (artifact->state != CORE_ARTIFACT_MOVED) {
        artifact->state = CORE_ARTIFACT_CANCELLED;
    }
}

#include "kaos_artifact.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t contents[CORE_ARTIFACT_MAX_BYTES];
static uint8_t staging[CORE_ARTIFACT_MAX_BYTES];
static const uint8_t etag[] = {0x21, 0x43, 0x65, 0x87};

static void clean(core_artifact *artifact, core_lifetime *owner, size_t size)
{
    core_lifetime_close(owner);
    core_artifact_cancel(artifact);
    assert(core_lifetime_unclaimed(owner));
    assert(core_lifetime_uncharge(owner, size));
    assert(core_lifetime_reclaimed(owner));
}

static void size_boundaries(void)
{
    const size_t sizes[] = {1, 15, 16, 17, 255, 256, 257, 1024, CORE_ARTIFACT_MAX_BYTES};
    for (size_t n = 0; n < sizeof(sizes) / sizeof(sizes[0]); ++n) {
        const size_t size = sizes[n];
        core_lifetime owner = CORE_LIFETIME_INIT(CORE_ARTIFACT_MAX_BYTES);
        core_artifact artifact = {0};
        assert(core_lifetime_charge(&owner, size));
        assert(core_artifact_begin(&artifact, &owner, staging, sizeof(staging), size,
                                    core_artifact_crc32(contents, size), 1, 0, 10000));
        uint32_t next;
        uint64_t token;
        for (size_t offset = 0; offset < size;) {
            assert(core_artifact_next(&artifact, 1, &next, &token));
            assert((next >> 4) * CORE_ARTIFACT_BLOCK_BYTES == offset && token == 1);
            const size_t count = size - offset > CORE_ARTIFACT_BLOCK_BYTES ?
                                 CORE_ARTIFACT_BLOCK_BYTES : size - offset;
            const bool more = offset + count < size;
            const uint32_t block = next | (more ? 8 : 0);
            assert(core_artifact_receive(&artifact, token, block, etag, sizeof(etag),
                                         contents + offset, count, 1) ==
                   (more ? CORE_ARTIFACT_MORE : CORE_ARTIFACT_COMPLETE));
            if (more) {
                assert(core_artifact_receive(&artifact, token, block, etag, sizeof(etag),
                                             contents + offset, count, 1) == CORE_ARTIFACT_IGNORED);
            }
            offset += count;
        }
        assert(artifact.state == CORE_ARTIFACT_READY && memcmp(staging, contents, size) == 0);
        assert(!core_artifact_next(&artifact, 1, &next, &token));
        uint8_t *loaded = NULL;
        size_t loaded_size = 0;
        assert(core_artifact_take(&artifact, 2, &loaded, &loaded_size));
        assert(loaded == staging && loaded_size == size && artifact.storage == NULL);
        assert(core_lifetime_unclaimed(&owner) && owner.bytes == size);
        assert(!core_artifact_take(&artifact, 2, &loaded, &loaded_size));
        assert(core_artifact_receive(&artifact, 1, 4, etag, sizeof(etag), contents, 1, 2) == CORE_ARTIFACT_IGNORED);
        clean(&artifact, &owner, size);
    }
}

static void negotiation_restart_and_late_blocks(void)
{
    const size_t size = 35;
    core_lifetime owner = CORE_LIFETIME_INIT(35);
    core_artifact artifact = {0};
    assert(core_lifetime_charge(&owner, size));
    assert(core_artifact_begin(&artifact, &owner, staging, sizeof(staging), size,
                                core_artifact_crc32(contents, size), 10, 0, 100));
    assert(core_artifact_receive(&artifact, 10, 0x08, etag, sizeof(etag), contents, 16, 1) == CORE_ARTIFACT_MORE);
    uint32_t next;
    uint64_t token;
    assert(core_artifact_next(&artifact, 1, &next, &token) && next == 0x10);
    assert(core_artifact_receive(&artifact, 10, 0x19, etag, sizeof(etag), contents + 16, 16, 2) == CORE_ARTIFACT_INVALID);
    assert(!core_artifact_restart(&artifact, 10, 3));
    assert(core_artifact_restart(&artifact, 11, 3));
    assert(artifact.deadline_us == 100 && artifact.received_size == 0);
    memset(staging, 0xa5, size);
    assert(core_artifact_receive(&artifact, 10, 0x08, etag, sizeof(etag), contents, 16, 4) == CORE_ARTIFACT_IGNORED);
    assert(staging[0] == 0xa5 && artifact.received_size == 0);
    assert(core_artifact_receive(&artifact, 11, 0x08, etag, sizeof(etag), contents, 16, 4) == CORE_ARTIFACT_MORE);
    uint8_t different_etag[] = {1};
    assert(core_artifact_receive(&artifact, 11, 0x18, different_etag, 1, contents + 16, 16, 4) == CORE_ARTIFACT_INVALID);
    assert(artifact.received_size == 16);
    assert(core_artifact_restart(&artifact, 12, 5));
    assert(core_artifact_receive(&artifact, 12, 0x08, etag, sizeof(etag), contents, 16, 5) == CORE_ARTIFACT_MORE);
    assert(core_artifact_receive(&artifact, 12, 0x18, etag, sizeof(etag), contents + 16, 16, 5) == CORE_ARTIFACT_MORE);
    assert(core_artifact_receive(&artifact, 12, 0x20, etag, sizeof(etag), contents + 32, 3, 5) == CORE_ARTIFACT_COMPLETE);
    uint8_t *loaded;
    size_t loaded_size;
    assert(!core_artifact_take(&artifact, 100, &loaded, &loaded_size));
    clean(&artifact, &owner, size);
}

static void staged_crc_and_partial_cleanup(void)
{
    const size_t size = 20;
    core_lifetime owner = CORE_LIFETIME_INIT(20);
    core_artifact artifact = {0};
    assert(core_lifetime_charge(&owner, size));
    assert(core_artifact_begin(&artifact, &owner, staging, sizeof(staging), size,
                                core_artifact_crc32(contents, size), 1, 0, 100));
    assert(core_artifact_receive(&artifact, 1, 0x08, etag, sizeof(etag), contents, 16, 1) == CORE_ARTIFACT_MORE);
    staging[0] ^= 1; /* Corruption AFTER reception must still fail final validation. */
    assert(core_artifact_receive(&artifact, 1, 0x10, etag, sizeof(etag), contents + 16, 4, 2) == CORE_ARTIFACT_INVALID);
    assert(owner.bytes == size && !core_lifetime_unclaimed(&owner));
    assert(core_artifact_restart(&artifact, 2, 3));
    assert(core_artifact_receive(&artifact, 2, 0x00, etag, sizeof(etag), contents, 16, 3) == CORE_ARTIFACT_INVALID);
    assert(core_artifact_restart(&artifact, 3, 4));
    assert(core_artifact_receive(&artifact, 3, 0x08, etag, sizeof(etag), contents, 16, 4) == CORE_ARTIFACT_MORE);
    core_lifetime_close(&owner);
    assert(!core_lifetime_unclaimed(&owner));
    const uint8_t before = staging[16];
    assert(core_artifact_receive(&artifact, 3, 0x10, etag, sizeof(etag), contents + 16, 4, 5) == CORE_ARTIFACT_IGNORED);
    assert(staging[16] == before);
    clean(&artifact, &owner, size);
    assert(core_lifetime_reopen(&owner));
    assert(core_lifetime_charge(&owner, size));
    assert(!core_artifact_begin(&artifact, &owner, staging, sizeof(staging), size,
                                 core_artifact_crc32(contents, size), 3, 6, 100));
    assert(core_artifact_begin(&artifact, &owner, staging, sizeof(staging), size,
                                core_artifact_crc32(contents, size), 4, 6, 100));
    assert(core_artifact_receive(&artifact, 3, 0x10, etag, sizeof(etag), contents + 16, 4, 7) == CORE_ARTIFACT_IGNORED);
    assert(!core_artifact_restart(&artifact, 5, 100));
    assert(core_artifact_receive(&artifact, 4, 4, etag, sizeof(etag), contents, size, 100) == CORE_ARTIFACT_INVALID);
    clean(&artifact, &owner, size);
}

static void invalid_admission_and_access_exhaustion(void)
{
    core_lifetime owner = CORE_LIFETIME_INIT(CORE_ARTIFACT_MAX_BYTES);
    core_artifact artifact = {0};
    assert(!core_artifact_begin(&artifact, &owner, staging, sizeof(staging), 1, 0, 1, 0, 10));
    assert(core_lifetime_charge(&owner, 1));
    assert(!core_artifact_begin(&artifact, &owner, staging, 0, 1, 0, 1, 0, 10));
    assert(!core_artifact_begin(&artifact, &owner, staging, sizeof(staging), 0, 0, 1, 0, 10));
    assert(!core_artifact_begin(&artifact, &owner, staging, sizeof(staging),
                                 CORE_ARTIFACT_MAX_BYTES + 1, 0, 1, 0, 10));
    assert(core_artifact_begin(&artifact, &owner, staging, sizeof(staging), 1, 0, 1, 0, 10));
    owner.last_ticket = UINT64_MAX;
    assert(!core_artifact_restart(&artifact, 2, 1));
    assert(core_lifetime_unclaimed(&owner));
    clean(&artifact, &owner, 1);
}

int main(void)
{
    assert(core_artifact_crc32((const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926));
    assert(core_artifact_crc32(NULL, 0) == 0);
    for (size_t i = 0; i < sizeof(contents); ++i) {
        contents[i] = (uint8_t)(i * 31 + i / 7);
    }
    size_boundaries();
    negotiation_restart_and_late_blocks();
    staged_crc_and_partial_cleanup();
    invalid_admission_and_access_exhaustion();
    puts("Artifact length/CRC, Block2, cancellation and ownership tests passed");
    return 0;
}

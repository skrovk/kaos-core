#ifndef KAOS_WAMR_H
#define KAOS_WAMR_H

#include "kaos_runtime.h"

/* Provisional qualification profiles, not measured ESP32 capacity claims.
 * Two persistent workers reserve their native stacks independently of node
 * arenas. An arena is charged in full until disposal; its internal allocations
 * cannot exceed the supplied limit. */
#ifdef ESP_PLATFORM
#define CORE_WAMR_ARENA_BYTES 90112u
#ifdef CORE_FOUR_NODE_PROFILE
/* P4 qualification selects a smaller fixed native stack; interpreter and
 * guest stacks remain separately bounded inside each declared arena. */
#define CORE_WAMR_NATIVE_STACK_BYTES 8192u
#else
#define CORE_WAMR_NATIVE_STACK_BYTES 16384u
#endif
#define CORE_WAMR_WORKER_PRIORITY 2u
#else
#define CORE_WAMR_ARENA_BYTES 131072u
#define CORE_WAMR_NATIVE_STACK_BYTES 262144u
#endif
#define CORE_WAMR_GLOBAL_BYTES 32768u
#define CORE_WAMR_ERROR_CAPACITY 4u
#define CORE_WAMR_MAX_WAIT_US 1000000u

/* One process-wide WAMR service. The callback must be bounded/nonblocking;
 * e.g. write a byte to a nonblocking event pipe. Initialization/shutdown belong
 * to process startup/shutdown, never an operation's control transition. */
bool core_wamr_init(core_node_runtime *out, void (*notify)(void *), void *context);
void core_wamr_shutdown(void);

/* Attach once after init, before preparing nodes. Callbacks run without the
 * runtime mailbox lock and must perform bounded copies under the registry
 * lock. Slot identity is trusted; guests supply only logical channel triples.
 * No callback may retain guest memory. Results use the endpoint service's
 * codes: 0 success, -1 disabled, -2 full, -3 empty, -4 invalid, -5 size.
 * get returns the copied byte count on success, including zero-byte messages.
 * event_get returns one coalesced state with PUT=1 and GET=2 permission bits. */
typedef struct {
    void *context;
    int32_t (*put)(void *, size_t, const uint64_t channel[3], const uint8_t *, size_t);
    int32_t (*get)(void *, size_t, const uint64_t channel[3], uint8_t *, size_t);
    int32_t (*event_get)(void *, size_t, uint64_t channel[3], uint32_t *permissions);
    bool (*event_ready)(void *, size_t);
} core_wamr_channels;
/* Guest imports, all under module "kaos":
 * put(channel_offset, data_offset, size) -> status
 * get(channel_offset, data_offset, capacity) -> byte count or negative status
 * event_get(data_offset, capacity) -> status
 * Channel arguments hold three little-endian u64s: source, destination, service.
 * Events hold the same 24 bytes, little-endian u32 permissions, then four zeros.
 * event_wait(timeout_us) returns 0 while an event is pending, 1 on timeout,
 * -1 on invalid timeout/cancellation; it never consumes an event. */
bool core_wamr_attach_channels(const core_wamr_channels *channels);
/* Attach before workers receive artifacts. entry_lock takes the registry lock
 * and returns startup permission, retaining the lock even on false. WAMR then
 * checks its mailbox and marks execution; entry_unlock releases the registry.
 * Both callbacks run without a held mailbox lock. The guard samples current
 * time after taking the registry lock. Neither callback spans guest code. */
bool core_wamr_attach_entry_guard(void *context,
    bool (*entry_lock)(void *, size_t), void (*entry_unlock)(void *));
/* Bounded wakeup only; safe while holding the registry lock. The unread event
 * remains in the endpoint service until event_get consumes it. */
void core_wamr_channel_changed(size_t slot);

/* Restricted bytecode profile checked before WAMR instantiation. WAMR still
 * performs full bytecode/type validation. Useful for artifact qualification. */
bool core_wamr_profile(const uint8_t *bytes, size_t size);

#endif

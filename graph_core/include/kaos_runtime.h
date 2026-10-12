#ifndef KAOS_RUNTIME_H
#define KAOS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One reserved mailbox per admission node slot. Runtime-owned state is only
 * inspected while lock() is held. The engine writes the three commands and
 * consumes init_pending; the host diagnostic sink consumes errors_pending.
 * The worker publishes the remaining state.
 * The same lock serializes actual exit/usability with INIT acceptance. Worker
 * code never changes admission, lifetime accounting or operation evidence. */
typedef struct {
    uint64_t start_deadline_us; /* original deadline, checked again at guest entry */
    bool start_requested;
    bool cancel_requested;
    bool dispose_requested;
    bool prepared;
    bool prepare_failed;
    bool configuration_active;
    bool main_active;
    bool init_pending;
    bool exited;
    bool fault;
    bool disposed;
    uint8_t errors_pending;
} core_runtime_state;

typedef struct {
    void *context;
    /* Queue inert preparation in the fixed slot. Success transfers artifact
     * ownership; failure leaves it with the caller and starts no worker work.
     * Copy the bounded configuration before returning. arena_bytes is a hard
     * runtime allocation limit, separately charged from artifact_size.
     * disposed means WAMR, artifact and arena are actually destroyed, all
     * imports/callbacks have returned, and no worker retains their addresses.
     * Slot mailbox storage and native task stack are fixed profile overhead. */
    bool (*prepare)(void *context, size_t slot, uint8_t *artifact,
                    size_t artifact_size, const uint8_t *configuration,
                    size_t configuration_size, size_t arena_bytes);
    core_runtime_state *(*lock)(void *context, size_t slot);
    /* Unlock also wakes a worker when a command has changed. */
    void (*unlock)(void *context, size_t slot);
} core_node_runtime;

#endif

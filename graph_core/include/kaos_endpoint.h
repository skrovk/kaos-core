#ifndef KAOS_ENDPOINT_H
#define KAOS_ENDPOINT_H

#include "kaos_admission.h"

/* All calls, including admission and closure, share the caller's short registry
 * lock. Copies are bounded, and no function retains caller memory or waits. */
typedef enum {
    CORE_DATA_OK = 0,
    CORE_DATA_DISABLED = -1,
    CORE_DATA_FULL = -2,
    CORE_DATA_EMPTY = -3,
    CORE_DATA_INVALID = -4,
    CORE_DATA_SIZE = -5
} core_data_result;

enum { CORE_CHANNEL_PUT = 1, CORE_CHANNEL_GET = 2 };
typedef struct {
    core_channel_id channel;
    uint8_t permissions;
} core_channel_event;

typedef struct {
    core_channel_id channel;
    core_op_id creating_op;
    core_instance_id source_instance, destination_instance;
    uint16_t size;
    uint8_t payload[CORE_MAX_PAYLOAD_BYTES];
} core_data_frame;

/* Network byte order: version:u8, tag:u8(1), six u64 identity fields,
 * payload length:u16, followed by exactly that many payload bytes. */
#define CORE_DATA_HEADER_BYTES 52
#define CORE_DATA_FRAME_BYTES (CORE_DATA_HEADER_BYTES + CORE_MAX_PAYLOAD_BYTES)

typedef struct {
    uint8_t payload[CORE_MAX_QUEUE_MESSAGES][CORE_MAX_PAYLOAD_BYTES];
    uint16_t sizes[CORE_MAX_QUEUE_MESSAGES];
    uint8_t head, count;
    bool prepared, active, closed;
} core_endpoint_queue;

typedef struct {
    core_admission *admission;
    core_endpoint_queue queues[CORE_MAX_ENDPOINT_RESERVATIONS];
    size_t next_sender;
    void (*notify)(void *context, size_t node_slot);
    void *context;
} core_endpoint_service;

void core_endpoint_init(core_endpoint_service *service, core_admission *admission,
                        void (*notify)(void *, size_t), void *context);
/* Run after admission/node transitions and before releasing the registry lock,
 * so closing an instance also publishes its endpoint permission changes.
 * Preparation is independent of runtime execution. Cleanup waits for accesses,
 * releases the queue charge and endpoint reservation, then publishes CLEANED. */
void core_endpoint_step(core_endpoint_service *service);
/* Engine validates commit, receiver-first ordering and exact authorization. */
bool core_endpoint_activate(core_endpoint_service *service, const core_binding *binding,
                            bool sender);
uint8_t core_endpoint_facts(const core_endpoint_service *service, const core_binding *binding,
                            bool sender);
void core_endpoint_close(core_endpoint_service *service, const core_binding *binding);

core_data_result core_channel_put(core_endpoint_service *service, core_instance_id caller,
                                  core_channel_id channel, const void *data, size_t size);
core_data_result core_channel_get(core_endpoint_service *service, core_instance_id caller,
                                  core_channel_id channel, void *data, size_t capacity,
                                  size_t *written);
core_data_result core_channel_event_get(core_endpoint_service *service, core_instance_id caller,
                                        core_channel_event *event);
bool core_channel_event_pending(const core_endpoint_service *service, core_instance_id caller);
/* Removes one message for one best-effort transfer attempt. Local destinations
 * use receive too. Returned frame owns its bytes and holds no endpoint access. */
bool core_endpoint_next(core_endpoint_service *service, uint64_t *destination_owner,
                        core_data_frame *frame);
core_data_result core_endpoint_receive(core_endpoint_service *service, uint64_t sender_owner,
                                       const core_data_frame *frame);
bool core_data_encode(const core_data_frame *frame, uint8_t *data, size_t capacity,
                      size_t *written);
bool core_data_decode(const uint8_t *data, size_t size, core_data_frame *frame);

#endif

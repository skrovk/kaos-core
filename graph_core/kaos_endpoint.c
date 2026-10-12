#include "kaos_endpoint.h"

#include <string.h>

static core_channel_id channel_id(const core_binding *binding)
{
    return (core_channel_id){binding->channel.source_logical,
                             binding->channel.destination_logical, binding->channel.service};
}

static bool same_channel(core_channel_id a, core_channel_id b)
{
    return a.source == b.source && a.destination == b.destination && a.service == b.service;
}

static bool same_binding(const core_binding *a, const core_binding *b)
{
    const core_channel *x = &a->channel, *y = &b->channel;
    return same_channel(channel_id(a), channel_id(b)) &&
        a->creating_op.value == b->creating_op.value &&
        x->source_instance.value == y->source_instance.value &&
        x->destination_instance.value == y->destination_instance.value &&
        x->source_owner == y->source_owner && x->destination_owner == y->destination_owner &&
        x->source_port == y->source_port && x->destination_port == y->destination_port &&
        x->type == y->type && x->payload_bytes == y->payload_bytes &&
        x->queue_messages == y->queue_messages && x->addition_allowance_us == y->addition_allowance_us;
}

static bool node_kind(uint8_t kind)
{
    return kind == CORE_REQUEST_ADD_NODE_NO_EDGES || kind == CORE_REQUEST_ADD_NODE ||
           kind == CORE_REQUEST_REMOVE_NODE_NO_EDGES || kind == CORE_REQUEST_REMOVE_NODE;
}

static void facts(core_endpoint_service *service, const core_binding *binding, bool sender,
                  uint8_t added)
{
    for (size_t r = 0; r < CORE_MAX_ADMISSION_RECORDS; ++r) {
        core_admission_record *record = &service->admission->records[r];
        if (!record->occupied) continue;
        for (size_t c = 0; c < record->scope.channel_count; ++c) {
            if (!(record->channel_known & (1u << c)) ||
                !same_binding(binding, &record->scope.bindings[c])) continue;
            const size_t object = (node_kind(record->scope.kind) ? 1u : 0u) + 2 * c + !sender;
            const uint8_t observed = record->scope.kind >= CORE_REQUEST_REMOVE_NODE_NO_EDGES ?
                (uint8_t)(added & ~(CORE_OBJECT_READY | CORE_OBJECT_ACTIVE)) : added;
            if ((record->local_facts[object] | observed) != record->local_facts[object])
                record->owner_delivered &= ~(UINT32_C(1) << object);
            record->local_facts[object] |= observed;
        }
    }
}

uint8_t core_endpoint_facts(const core_endpoint_service *service,
                            const core_binding *binding, bool sender)
{
    uint8_t result = 0;
    for (size_t r = 0; r < CORE_MAX_ADMISSION_RECORDS; ++r) {
        const core_admission_record *record = &service->admission->records[r];
        if (!record->occupied) continue;
        for (size_t c = 0; c < record->scope.channel_count; ++c) {
            if (!(record->channel_known & (1u << c)) ||
                !same_binding(binding, &record->scope.bindings[c])) continue;
            const size_t object = (node_kind(record->scope.kind) ? 1u : 0u) + 2 * c + !sender;
            result |= record->local_facts[object];
        }
    }
    return result;
}

static core_reserved_node *find_node(core_endpoint_service *service, core_instance_id instance)
{
    for (size_t n = 0; n < CORE_MAX_NODE_RESERVATIONS; ++n) {
        core_reserved_node *node = &service->admission->nodes[n];
        if (node->occupied && node->instance.value == instance.value) return node;
    }
    return NULL;
}

static core_reserved_node *endpoint_node(core_endpoint_service *service,
                                         const core_reserved_endpoint *endpoint)
{
    return find_node(service, endpoint->sender ? endpoint->binding.channel.source_instance :
                                               endpoint->binding.channel.destination_instance);
}

static void notification(core_endpoint_service *service,
                          const core_reserved_endpoint *endpoint, uint8_t permissions)
{
    core_reserved_node *node = endpoint_node(service, endpoint);
    if (node == NULL) return;
    for (size_t e = 0; e < CORE_MAX_REQUEST_CHANNELS; ++e) {
        core_channel_notification *event = &node->notifications[e];
        if (!event->occupied || event->creating_op.value != endpoint->binding.creating_op.value ||
            !same_channel(event->channel, channel_id(&endpoint->binding))) continue;
        event->permissions = permissions;
        event->pending = true;
        if (service->notify) service->notify(service->context,
                                             (size_t)(node - service->admission->nodes));
        return;
    }
}

void core_endpoint_init(core_endpoint_service *service, core_admission *admission,
                        void (*notify)(void *, size_t), void *context)
{
    *service = (core_endpoint_service){.admission = admission, .notify = notify, .context = context};
}

void core_endpoint_close(core_endpoint_service *service, const core_binding *binding)
{
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e) {
        core_reserved_endpoint *endpoint = &service->admission->endpoints[e];
        if (endpoint->occupied && same_binding(binding, &endpoint->binding)) {
            core_lifetime_close(&endpoint->lifetime);
            core_endpoint_queue *queue = &service->queues[e];
            if (!queue->closed) {
                queue->closed = true;
                queue->active = false;
                notification(service, endpoint, 0);
            }
        }
    }
}

void core_endpoint_step(core_endpoint_service *service)
{
    core_admission *pool = service->admission;
    /* Close all affected local roles first. A peer role may precede the exited
     * node's role in the array; it must still drain in this same bounded pass. */
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e) {
        core_reserved_endpoint *endpoint = &pool->endpoints[e];
        if (!endpoint->occupied || endpoint->lifetime.closing) continue;
        const core_reserved_node *node = endpoint_node(service, endpoint);
        if (node == NULL || node->lifetime.closing) {
            facts(service, &endpoint->binding, endpoint->sender, CORE_OBJECT_FAILED);
            /* A co-located binding has no usable transfer path after either
             * node exits. Reclaim both local endpoint roles, preserving the
             * peer node/runtime. This issues no distributed removal. */
            core_endpoint_close(service, &endpoint->binding);
        }
    }
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e) {
        core_reserved_endpoint *endpoint = &pool->endpoints[e];
        core_endpoint_queue *queue = &service->queues[e];
        if (!endpoint->occupied) continue;
        if (endpoint->lifetime.closing) {
            if (!queue->closed) {
                queue->closed = true;
                queue->active = false;
                notification(service, endpoint, 0);
            }
            facts(service, &endpoint->binding, endpoint->sender, CORE_OBJECT_CLEANUP);
            if (!core_lifetime_unclaimed(&endpoint->lifetime) ||
                endpoint->prepare.phase != CORE_ACTION_EMPTY) continue;
            facts(service, &endpoint->binding, endpoint->sender, CORE_OBJECT_STOPPED);
            queue->count = queue->head = 0;
            if (queue->prepared && core_lifetime_uncharge(&endpoint->lifetime, CORE_ENDPOINT_BYTES))
                queue->prepared = false;
        } else if (!queue->prepared) {
            if (!core_lifetime_charge(&endpoint->lifetime, CORE_ENDPOINT_BYTES)) {
                facts(service, &endpoint->binding, endpoint->sender, CORE_OBJECT_FAILED);
                core_lifetime_close(&endpoint->lifetime);
                continue;
            }
            queue->head = queue->count = 0;
            queue->active = queue->closed = false;
            queue->prepared = true;
            facts(service, &endpoint->binding, endpoint->sender, CORE_OBJECT_READY);
        }
    }
    /* Admission releases an exact binding only after every co-located role is
     * reclaimed. Retain evidence in each immutable scope before slot reuse. */
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e) {
        const core_reserved_endpoint *endpoint = &pool->endpoints[e];
        if (!endpoint->occupied || !core_lifetime_reclaimed(&endpoint->lifetime)) continue;
        const core_binding binding = endpoint->binding;
        if (!core_admission_release_binding(pool, &binding)) continue;
        for (size_t q = 0; q < CORE_MAX_ENDPOINT_RESERVATIONS; ++q) {
            if (!same_binding(&binding, &pool->endpoints[q].binding)) continue;
            service->queues[q].prepared = service->queues[q].active = service->queues[q].closed = false;
            service->queues[q].head = service->queues[q].count = 0;
        }
        if (binding.channel.source_owner == pool->local_device)
            facts(service, &binding, true, CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED);
        if (binding.channel.destination_owner == pool->local_device)
            facts(service, &binding, false, CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED);
    }
    /* An authorized cleanup fence also covers work that was never prepared. */
    for (size_t r = 0; r < CORE_MAX_ADMISSION_RECORDS; ++r) {
        core_admission_record *record = &pool->records[r];
        if (!record->occupied) continue;
        for (size_t c = 0; c < record->scope.channel_count; ++c) {
            if (!(record->channel_known & (1u << c)) ||
                !(record->channel_cleanup & (1u << c))) continue;
            const core_binding *binding = &record->scope.bindings[c];
            for (unsigned role = 0; role < 2; ++role) {
                const bool sender = role == 0;
                const uint64_t owner = sender ? binding->channel.source_owner :
                                                binding->channel.destination_owner;
                if (owner != pool->local_device) continue;
                bool present = false;
                for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e)
                    present |= pool->endpoints[e].occupied && pool->endpoints[e].sender == sender &&
                               same_binding(binding, &pool->endpoints[e].binding);
                if (!present) facts(service, binding, sender,
                    CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED);
            }
        }
    }
}

bool core_endpoint_activate(core_endpoint_service *service, const core_binding *binding, bool sender)
{
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e) {
        const core_reserved_endpoint *endpoint = &service->admission->endpoints[e];
        core_endpoint_queue *queue = &service->queues[e];
        if (!endpoint->occupied || endpoint->sender != sender ||
            !same_binding(binding, &endpoint->binding)) continue;
        const core_reserved_node *node = endpoint_node(service, endpoint);
        if (!queue->prepared || endpoint->lifetime.closing || node == NULL || node->lifetime.closing)
            return false;
        if (!queue->active) {
            queue->active = true;
            notification(service, endpoint, sender ? CORE_CHANNEL_PUT : CORE_CHANNEL_GET);
        }
        facts(service, binding, sender, CORE_OBJECT_READY | CORE_OBJECT_ACTIVE);
        return true;
    }
    return false;
}

static core_reserved_endpoint *access_endpoint(core_endpoint_service *service,
                                                core_instance_id caller, core_channel_id channel,
                                                bool sender, core_access *access)
{
    const core_reserved_node *node = find_node(service, caller);
    if (node == NULL || node->lifetime.closing) return NULL;
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e) {
        core_reserved_endpoint *endpoint = &service->admission->endpoints[e];
        const core_instance_id instance = sender ? endpoint->binding.channel.source_instance :
                                                   endpoint->binding.channel.destination_instance;
        if (endpoint->occupied && endpoint->sender == sender && instance.value == caller.value &&
            same_channel(channel, channel_id(&endpoint->binding)) && service->queues[e].active &&
            core_access_acquire(&endpoint->lifetime, access)) return endpoint;
    }
    return NULL;
}

static void enqueue(core_endpoint_queue *queue, const core_channel *channel,
                     const void *data, size_t size)
{
    const size_t tail = (queue->head + queue->count) % channel->queue_messages;
    if (size) memcpy(queue->payload[tail], data, size);
    queue->sizes[tail] = (uint16_t)size;
    ++queue->count;
}

static void consume(core_endpoint_queue *queue, const core_channel *channel)
{
    queue->head = (uint8_t)((queue->head + 1) % channel->queue_messages);
    --queue->count;
}

core_data_result core_channel_put(core_endpoint_service *service, core_instance_id caller,
                                   core_channel_id channel, const void *data, size_t size)
{
    if (data == NULL && size != 0) return CORE_DATA_INVALID;
    core_access access;
    core_reserved_endpoint *endpoint = access_endpoint(service, caller, channel, true, &access);
    if (endpoint == NULL) return CORE_DATA_DISABLED;
    core_endpoint_queue *queue = &service->queues[endpoint - service->admission->endpoints];
    core_data_result result = CORE_DATA_OK;
    if (size > endpoint->binding.channel.payload_bytes) result = CORE_DATA_SIZE;
    else if (queue->count == endpoint->binding.channel.queue_messages) result = CORE_DATA_FULL;
    else enqueue(queue, &endpoint->binding.channel, data, size);
    (void)core_access_release(&endpoint->lifetime, access);
    return result;
}

core_data_result core_channel_get(core_endpoint_service *service, core_instance_id caller,
                                   core_channel_id channel, void *data, size_t capacity,
                                   size_t *written)
{
    if (written == NULL || (data == NULL && capacity != 0)) return CORE_DATA_INVALID;
    core_access access;
    core_reserved_endpoint *endpoint = access_endpoint(service, caller, channel, false, &access);
    if (endpoint == NULL) return CORE_DATA_DISABLED;
    core_endpoint_queue *queue = &service->queues[endpoint - service->admission->endpoints];
    core_data_result result = CORE_DATA_OK;
    if (queue->count == 0) result = CORE_DATA_EMPTY;
    else if (capacity < queue->sizes[queue->head]) result = CORE_DATA_SIZE;
    else {
        *written = queue->sizes[queue->head];
        if (*written) memcpy(data, queue->payload[queue->head], *written);
        consume(queue, &endpoint->binding.channel);
    }
    (void)core_access_release(&endpoint->lifetime, access);
    return result;
}

core_data_result core_channel_event_get(core_endpoint_service *service, core_instance_id caller,
                                         core_channel_event *out)
{
    if (out == NULL) return CORE_DATA_INVALID;
    core_reserved_node *node = find_node(service, caller);
    if (node == NULL || node->lifetime.closing) return CORE_DATA_DISABLED;
    for (size_t e = 0; e < CORE_MAX_REQUEST_CHANNELS; ++e) {
        core_channel_notification *event = &node->notifications[e];
        if (!event->occupied || !event->pending) continue;
        *out = (core_channel_event){.channel = event->channel, .permissions = event->permissions};
        event->pending = false;
        if (!event->attached) *event = (core_channel_notification){0};
        return CORE_DATA_OK;
    }
    return CORE_DATA_EMPTY;
}

bool core_channel_event_pending(const core_endpoint_service *service, core_instance_id caller)
{
    for (size_t n = 0; n < CORE_MAX_NODE_RESERVATIONS; ++n) {
        const core_reserved_node *node = &service->admission->nodes[n];
        if (!node->occupied || node->instance.value != caller.value || node->lifetime.closing) continue;
        for (size_t e = 0; e < CORE_MAX_REQUEST_CHANNELS; ++e)
            if (node->notifications[e].occupied && node->notifications[e].pending) return true;
    }
    return false;
}

bool core_endpoint_next(core_endpoint_service *service, uint64_t *destination_owner,
                         core_data_frame *frame)
{
    if (destination_owner == NULL || frame == NULL) return false;
    for (size_t count = 0; count < CORE_MAX_ENDPOINT_RESERVATIONS; ++count) {
        const size_t e = service->next_sender;
        service->next_sender = (e + 1) % CORE_MAX_ENDPOINT_RESERVATIONS;
        core_reserved_endpoint *endpoint = &service->admission->endpoints[e];
        core_endpoint_queue *queue = &service->queues[e];
        const core_reserved_node *node = endpoint_node(service, endpoint);
        core_access access;
        if (!endpoint->occupied || !endpoint->sender || !queue->active || queue->count == 0 ||
            node == NULL || node->lifetime.closing ||
            !core_access_acquire(&endpoint->lifetime, &access)) continue;
        frame->channel = channel_id(&endpoint->binding);
        frame->creating_op = endpoint->binding.creating_op;
        frame->source_instance = endpoint->binding.channel.source_instance;
        frame->destination_instance = endpoint->binding.channel.destination_instance;
        frame->size = queue->sizes[queue->head];
        if (frame->size) memcpy(frame->payload, queue->payload[queue->head], frame->size);
        *destination_owner = endpoint->binding.channel.destination_owner;
        consume(queue, &endpoint->binding.channel);
        (void)core_access_release(&endpoint->lifetime, access);
        return true;
    }
    return false;
}

core_data_result core_endpoint_receive(core_endpoint_service *service, uint64_t sender_owner,
                                        const core_data_frame *frame)
{
    if (frame == NULL || frame->size > CORE_MAX_PAYLOAD_BYTES) return CORE_DATA_INVALID;
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e) {
        core_reserved_endpoint *endpoint = &service->admission->endpoints[e];
        const core_channel *channel = &endpoint->binding.channel;
        if (!endpoint->occupied || endpoint->sender ||
            !same_channel(frame->channel, channel_id(&endpoint->binding)) ||
            frame->creating_op.value != endpoint->binding.creating_op.value ||
            frame->source_instance.value != channel->source_instance.value ||
            frame->destination_instance.value != channel->destination_instance.value ||
            sender_owner != channel->source_owner) continue;
        const core_reserved_node *node = endpoint_node(service, endpoint);
        core_endpoint_queue *queue = &service->queues[e];
        core_access access;
        if (!queue->active || node == NULL || node->lifetime.closing ||
            !core_access_acquire(&endpoint->lifetime, &access)) return CORE_DATA_DISABLED;
        core_data_result result = CORE_DATA_OK;
        if (frame->size > channel->payload_bytes) result = CORE_DATA_SIZE;
        else if (queue->count == channel->queue_messages) result = CORE_DATA_FULL;
        else enqueue(queue, channel, frame->payload, frame->size);
        (void)core_access_release(&endpoint->lifetime, access);
        return result;
    }
    return CORE_DATA_DISABLED;
}

static void write64(uint8_t *data, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) data[i] = (uint8_t)(value >> (56 - 8 * i));
}

static uint64_t read64(const uint8_t *data)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | data[i];
    return value;
}

bool core_data_encode(const core_data_frame *frame, uint8_t *data, size_t capacity, size_t *written)
{
    if (frame == NULL || data == NULL || written == NULL || frame->size > CORE_MAX_PAYLOAD_BYTES ||
        capacity < CORE_DATA_HEADER_BYTES + (size_t)frame->size) return false;
    data[0] = CORE_SCHEMA_VERSION;
    data[1] = 1;
    write64(data + 2, frame->channel.source);
    write64(data + 10, frame->channel.destination);
    write64(data + 18, frame->channel.service);
    write64(data + 26, frame->creating_op.value);
    write64(data + 34, frame->source_instance.value);
    write64(data + 42, frame->destination_instance.value);
    data[50] = (uint8_t)(frame->size >> 8);
    data[51] = (uint8_t)frame->size;
    if (frame->size) memcpy(data + CORE_DATA_HEADER_BYTES, frame->payload, frame->size);
    *written = CORE_DATA_HEADER_BYTES + frame->size;
    return true;
}

bool core_data_decode(const uint8_t *data, size_t size, core_data_frame *frame)
{
    if (data == NULL || frame == NULL || size < CORE_DATA_HEADER_BYTES ||
        data[0] != CORE_SCHEMA_VERSION || data[1] != 1) return false;
    const size_t payload_size = ((size_t)data[50] << 8) | data[51];
    if (payload_size > CORE_MAX_PAYLOAD_BYTES || size != CORE_DATA_HEADER_BYTES + payload_size)
        return false;
    frame->channel = (core_channel_id){read64(data + 2), read64(data + 10), read64(data + 18)};
    frame->creating_op.value = read64(data + 26);
    frame->source_instance.value = read64(data + 34);
    frame->destination_instance.value = read64(data + 42);
    frame->size = (uint16_t)payload_size;
    if (payload_size) memcpy(frame->payload, data + CORE_DATA_HEADER_BYTES, payload_size);
    return true;
}

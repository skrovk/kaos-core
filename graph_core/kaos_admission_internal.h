#ifndef KAOS_ADMISSION_INTERNAL_H
#define KAOS_ADMISSION_INTERNAL_H
#include "kaos_admission.h"
#include "kaos_cbor.h"

/* Channel identity is independent of the creating operation and descriptor. */
static inline core_channel_id core_channel_identity(const core_channel *channel)
{
    return (core_channel_id){channel->source_logical, channel->destination_logical,
                             channel->service};
}

static inline bool core_channel_id_equal(core_channel_id a, core_channel_id b)
{
    return a.source == b.source && a.destination == b.destination && a.service == b.service;
}

static inline bool core_channel_same_identity(const core_channel *a, const core_channel *b)
{
    return a->source_logical == b->source_logical &&
           a->destination_logical == b->destination_logical && a->service == b->service;
}

static inline bool core_binding_same_identity(const core_binding *a, const core_binding *b)
{
    return core_channel_same_identity(&a->channel, &b->channel) &&
           a->creating_op.value == b->creating_op.value;
}

static inline bool core_channel_descriptor_equal(const core_channel *a, const core_channel *b)
{
    return core_channel_same_identity(a, b) &&
           a->source_instance.value == b->source_instance.value &&
           a->destination_instance.value == b->destination_instance.value &&
           a->source_owner == b->source_owner && a->destination_owner == b->destination_owner &&
           a->source_port == b->source_port && a->destination_port == b->destination_port &&
           a->type == b->type && a->payload_bytes == b->payload_bytes &&
           a->queue_messages == b->queue_messages &&
           a->addition_allowance_us == b->addition_allowance_us;
}

static inline bool core_binding_equal(const core_binding *a, const core_binding *b)
{
    return a->creating_op.value == b->creating_op.value &&
           core_channel_descriptor_equal(&a->channel, &b->channel);
}

/* Descriptor schema lives beside its decoder in kaos_request.c. */
void core_channel_encode(core_cbor_writer *writer, const core_channel *channel);

core_admit_result core_admission_reserve(core_admission *pool, core_op_id id,
                                         const core_node_request *node_request,
                                         const core_request_scope *scope, uint64_t now_us,
                                         bool endpoint_only, bool cancel,
                                         const core_admission_record **out);
core_admit_result core_admission_reserve_channel(core_admission *pool, uint64_t coordinator,
                                                 core_op_id id, uint8_t index,
                                                 const core_channel *channel, bool cancel,
                                                 const core_admission_record **out);
core_admit_result core_admission_reserve_removal(core_admission *pool, uint64_t coordinator,
                                                 core_op_id id, uint8_t index,
                                                 core_instance_id target,
                                                 const core_binding *binding,
                                                 const core_admission_record **out);
#endif

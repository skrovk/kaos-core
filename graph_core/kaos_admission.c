#include "kaos_admission_internal.h"

#include <string.h>

static bool reclaimable(const core_admission *pool, const core_admission_record *record);

static bool node_add(uint8_t kind)
{
    return kind == CORE_REQUEST_ADD_NODE_NO_EDGES || kind == CORE_REQUEST_ADD_NODE;
}

static bool node_remove(uint8_t kind)
{
    return kind == CORE_REQUEST_REMOVE_NODE_NO_EDGES || kind == CORE_REQUEST_REMOVE_NODE;
}

static bool removal(uint8_t kind)
{
    return node_remove(kind) || kind == CORE_REQUEST_REMOVE_EDGE;
}

static bool same_channel(const core_channel *a, const core_channel *b)
{
    return a->source_logical == b->source_logical &&
           a->destination_logical == b->destination_logical && a->service == b->service;
}

static bool same_binding(const core_binding *a, const core_binding *b)
{
    return same_channel(&a->channel, &b->channel) &&
           a->creating_op.value == b->creating_op.value;
}

static bool same_descriptor(const core_channel *a, const core_channel *b)
{
    return same_channel(a, b) &&
           a->source_instance.value == b->source_instance.value &&
           a->destination_instance.value == b->destination_instance.value &&
           a->source_owner == b->source_owner && a->destination_owner == b->destination_owner &&
           a->source_port == b->source_port && a->destination_port == b->destination_port &&
           a->type == b->type && a->payload_bytes == b->payload_bytes &&
           a->queue_messages == b->queue_messages &&
           a->addition_allowance_us == b->addition_allowance_us;
}

static bool incident(const core_channel *channel, uint64_t logical, core_instance_id instance)
{
    return (channel->source_logical == logical && channel->source_instance.value == instance.value) ||
           (channel->destination_logical == logical &&
            channel->destination_instance.value == instance.value);
}

static bool configured_owner(const core_admission *pool, uint64_t owner)
{
    for (size_t i = 0; i < pool->peer_count; ++i) {
        if (pool->peers[i] == owner) {
            return true;
        }
    }
    return false;
}

static core_reserved_node *find_node(core_admission *pool, core_instance_id instance)
{
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        if (pool->nodes[i].occupied && pool->nodes[i].instance.value == instance.value) {
            return &pool->nodes[i];
        }
    }
    return NULL;
}

static bool node_fenced(const core_admission *pool, uint64_t logical, core_instance_id instance)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        const core_admission_record *record = &pool->records[i];
        if (record->occupied && (node_remove(record->scope.kind) ||
            (record->cleanup_selected && node_add(record->scope.kind))) &&
            record->scope.logical_node_id == logical && record->scope.instance.value == instance.value) {
            return true;
        }
    }
    return false;
}

static bool binding_fenced(const core_admission *pool, const core_binding *binding)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        const core_admission_record *record = &pool->records[i];
        if (!record->occupied) {
            continue;
        }
        for (size_t j = 0; j < record->scope.channel_count; ++j) {
            const core_channel_set bit = (core_channel_set)(1u << j);
            if ((record->channel_known & bit) &&
                (removal(record->scope.kind) || record->cleanup_selected ||
                 (record->channel_cleanup & bit)) &&
                same_binding(&record->scope.bindings[j], binding)) {
                return true;
            }
        }
    }
    return false;
}

bool core_admission_init(core_admission *pool, size_t node_bytes, size_t byte_limit)
{
    if (node_bytes == 0) {
        return false;
    }
    *pool = (core_admission){ .node_bytes = node_bytes, .byte_limit = byte_limit };
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        pool->nodes[i].lifetime = (core_lifetime)CORE_LIFETIME_INIT(node_bytes);
        core_lifetime_close(&pool->nodes[i].lifetime);
    }
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        pool->endpoints[i].lifetime = (core_lifetime)CORE_LIFETIME_INIT(CORE_ENDPOINT_BYTES);
        core_lifetime_close(&pool->endpoints[i].lifetime);
    }
    return true;
}

bool core_admission_configure(core_admission *pool, uint64_t local_device,
                              uint64_t authority, const uint64_t *peers, size_t peer_count)
{
    if (pool->configured || peers == NULL || peer_count == 0 || peer_count > CORE_MAX_PEERS) {
        return false;
    }
    bool local_present = false;
    for (size_t i = 0; i < peer_count; ++i) {
        local_present |= peers[i] == local_device;
        for (size_t j = 0; j < i; ++j) {
            if (peers[i] == peers[j]) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        if (pool->records[i].occupied) {
            return false;
        }
    }
    if (!local_present) {
        return false;
    }
    pool->local_device = local_device;
    pool->authority = authority;
    pool->peer_count = (uint8_t)peer_count;
    memcpy(pool->peers, peers, peer_count * sizeof(*peers));
    pool->configured = true;
    return true;
}

const core_admission_record *core_admission_find(const core_admission *pool, core_op_id id)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        if (pool->records[i].occupied && pool->records[i].id.value == id.value) {
            return &pool->records[i];
        }
    }
    return NULL;
}

static core_admission_record *select_record(core_admission *pool)
{
    core_admission_record *candidate = NULL;
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *r = &pool->records[i];
        if (r->incarnation == UINT64_MAX) continue;
        if (!r->occupied) return r;
        if (!reclaimable(pool, r)) continue;
        const bool acknowledged = r->endpoint_only || !core_admission_report_retained(r);
        const bool previous_ack = candidate != NULL &&
            (candidate->endpoint_only || !core_admission_report_retained(candidate));
        if (candidate == NULL || (acknowledged && !previous_ack) ||
            (acknowledged == previous_ack && r->report_age != 0 &&
             (candidate->report_age == 0 || r->report_age < candidate->report_age)))
            candidate = r;
    }
    /* Merely select. Rejected admission must not evict or mutate anything. */
    return candidate;
}

static void publish_record(core_admission *pool, core_admission_record *record)
{
    if (record->occupied) (void)core_admission_forget(pool, record->id);
    const uint64_t incarnation = record->incarnation + 1;
    memset(record, 0, sizeof(*record));
    record->incarnation = incarnation;
}

static core_reserved_node *free_node(core_admission *pool)
{
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        core_reserved_node *node = &pool->nodes[i];
        if (!node->occupied && node->prepare.phase == CORE_ACTION_EMPTY &&
            node->lifetime.last_ticket != UINT64_MAX && core_lifetime_reclaimed(&node->lifetime)) {
            return node;
        }
    }
    return NULL;
}

static void reserve_bytes(core_admission *pool, size_t bytes)
{
    pool->reserved_bytes += bytes;
    if (pool->reserved_bytes > pool->peak_reserved_bytes) {
        pool->peak_reserved_bytes = pool->reserved_bytes;
    }
}

core_admit_result core_node_admit(core_admission *pool, core_op_id id,
                                  uint64_t now_us, uint64_t allowance_us,
                                  const core_admission_record **out)
{
    if (core_admission_find(pool, id) != NULL) {
        return CORE_ADMIT_DUPLICATE;
    }
    uint64_t deadline;
    if (!core_deadline(now_us, allowance_us, &deadline)) {
        return CORE_ADMIT_INVALID;
    }
    core_admission_record *record = select_record(pool);
    core_reserved_node *node = free_node(pool);
    if (pool->node_bytes > pool->byte_limit - pool->reserved_bytes || record == NULL || node == NULL) {
        return CORE_ADMIT_FULL;
    }
    publish_record(pool, record);
    (void)core_lifetime_reopen(&node->lifetime);
    node->instance = (core_instance_id){ .value = id.value };
    node->logical_node_id = 0;
    node->port_count = 0;
    memset(node->notifications, 0, sizeof(node->notifications));
    node->established = false;
    node->occupied = true;
    record->id = id;
    record->deadline_us = deadline;
    record->node = node;
    record->occupied = true;
    reserve_bytes(pool, pool->node_bytes);
    *out = record;
    return CORE_ADMIT_ACCEPTED;
}

static bool port_matches(const core_port *ports, size_t count, uint16_t id,
                         uint8_t direction, uint16_t type, uint16_t payload)
{
    for (size_t i = 0; i < count; ++i) {
        if (ports[i].id == id) {
            return ports[i].direction == direction && ports[i].type == type &&
                   payload <= ports[i].max_payload;
        }
    }
    return false;
}

/* The remote owner repeats these checks against its actual node registry when
 * it accepts issued endpoint preparation. O's descriptor is not a reservation. */
static bool local_port_matches(core_admission *pool, const core_request_scope *scope,
                               const core_channel *channel, bool sender)
{
    const uint64_t owner = sender ? channel->source_owner : channel->destination_owner;
    if (owner != pool->local_device) {
        return true;
    }
    const uint64_t logical = sender ? channel->source_logical : channel->destination_logical;
    const core_instance_id instance = sender ? channel->source_instance : channel->destination_instance;
    const uint16_t port = sender ? channel->source_port : channel->destination_port;
    const uint8_t direction = sender ? CORE_PORT_OUTPUT : CORE_PORT_INPUT;
    if (node_fenced(pool, logical, instance)) {
        return false;
    }
    if (node_add(scope->kind) && instance.value == scope->instance.value &&
        logical == scope->logical_node_id) {
        return port_matches(scope->ports, scope->port_count, port, direction,
                            channel->type, channel->payload_bytes);
    }
    const core_reserved_node *node = find_node(pool, instance);
    return node != NULL && node->logical_node_id == logical && node->established &&
           !node->lifetime.closing &&
           port_matches(node->ports, node->port_count, port, direction,
                        channel->type, channel->payload_bytes);
}

static bool scope_contains(const core_request_scope *scope, core_channel_set known,
                            const core_binding *binding)
{
    for (size_t i = 0; i < scope->channel_count; ++i) {
        if ((known & (1u << i)) && same_binding(&scope->bindings[i], binding) &&
            same_descriptor(&scope->bindings[i].channel, &binding->channel)) {
            return true;
        }
    }
    return false;
}

static size_t endpoint_object(const core_admission_record *record, size_t channel,
                              unsigned role)
{
    return (node_add(record->scope.kind) || node_remove(record->scope.kind) ? 1u : 0u) +
        2 * channel + role;
}

/* Scope bookkeeping may use accepted closure from another local coordinator
 * observer. This does not update either observer's independently correlated
 * result. Ki owns only its local role and has no remote closure obligation. */
static bool remote_closed(const core_admission *pool, const core_binding *binding,
                          unsigned role)
{
    for (size_t r = 0; r < CORE_MAX_ADMISSION_RECORDS; ++r) {
        const core_admission_record *record = &pool->records[r];
        if (!record->occupied || record->endpoint_only) continue;
        for (size_t c = 0; c < record->scope.channel_count; ++c) {
            if ((record->channel_known & (1u << c)) &&
                same_binding(binding, &record->scope.bindings[c]) &&
                same_descriptor(&binding->channel, &record->scope.bindings[c].channel) &&
                ((record->evidence.objects[endpoint_object(record, c, role)] & CORE_OBJECT_CLEANED) ||
                 (record->closed_objects & (UINT32_C(1) << endpoint_object(record, c, role)))))
                return true;
        }
    }
    return false;
}

static bool retained_incident(const core_admission *pool,
                              const core_admission_record *record, size_t channel)
{
    const core_binding *binding = &record->scope.bindings[channel];
    for (unsigned role = 0; role < 2; ++role) {
        const uint64_t owner = role == 0 ? binding->channel.source_owner :
                                          binding->channel.destination_owner;
        if (record->endpoint_only) {
            if (owner == pool->local_device &&
                !(record->local_facts[endpoint_object(record, channel, role)] & CORE_OBJECT_CLEANED))
                return true;
        } else if (owner != pool->local_device && !remote_closed(pool, binding, role)) {
            return true;
        }
    }
    /* Live local roles are counted directly below. Their absence means the
     * admission-owned reservation was released, even in resource-only callers
     * which do not run the endpoint evidence service. */
    return false;
}

static bool include_binding(const core_binding **bindings, size_t *count,
                            const core_binding *binding)
{
    for (size_t i = 0; i < *count; ++i)
        if (same_binding(bindings[i], binding)) return true;
    if (*count == CORE_MAX_REQUEST_CHANNELS) return false;
    bindings[(*count)++] = binding;
    return true;
}

/* Complete current incidence includes retained remote obligations after local
 * endpoint reclamation. Fixed profile bounds keep the snapshot on the stack. */
static bool current_incidence(const core_admission *pool, uint64_t logical,
                              core_instance_id instance, const core_binding **bindings,
                              size_t *count)
{
    *count = 0;
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        const core_reserved_endpoint *endpoint = &pool->endpoints[i];
        if (endpoint->occupied && incident(&endpoint->binding.channel, logical, instance) &&
            !include_binding(bindings, count, &endpoint->binding)) return false;
    }
    for (size_t r = 0; r < CORE_MAX_ADMISSION_RECORDS; ++r) {
        const core_admission_record *record = &pool->records[r];
        if (!record->occupied) continue;
        for (size_t c = 0; c < record->scope.channel_count; ++c) {
            const core_binding *binding = &record->scope.bindings[c];
            if ((record->channel_known & (1u << c)) &&
                incident(&binding->channel, logical, instance) &&
                retained_incident(pool, record, c) && !include_binding(bindings, count, binding))
                return false;
        }
    }
    return true;
}

static bool incidence_fits(const core_admission *pool, const core_request_scope *scope,
                           uint64_t logical, core_instance_id instance)
{
    const core_binding *bindings[CORE_MAX_REQUEST_CHANNELS];
    size_t count;
    if (!current_incidence(pool, logical, instance, bindings, &count)) return false;
    for (size_t i = 0; i < scope->channel_count; ++i) {
        if (incident(&scope->bindings[i].channel, logical, instance) &&
            !include_binding(bindings, &count, &scope->bindings[i])) return false;
    }
    return true;
}

static bool notification_matches(const core_channel_notification *event,
                                  const core_channel *channel)
{
    return event->occupied && event->channel.source == channel->source_logical &&
           event->channel.destination == channel->destination_logical &&
           event->channel.service == channel->service;
}

static bool notifications_fit(const core_reserved_node *node,
                               const core_request_scope *scope)
{
    size_t needed = 0;
    for (size_t i = 0; i < CORE_MAX_REQUEST_CHANNELS; ++i)
        needed += node->notifications[i].occupied;
    for (size_t i = 0; i < scope->channel_count; ++i) {
        const core_channel *channel = &scope->bindings[i].channel;
        if (!incident(channel, node->logical_node_id, node->instance)) continue;
        bool existing = false;
        for (size_t j = 0; j < CORE_MAX_REQUEST_CHANNELS; ++j)
            existing |= notification_matches(&node->notifications[j], channel);
        if (!existing) ++needed;
    }
    return needed <= CORE_MAX_REQUEST_CHANNELS;
}

static void reserve_notification(core_reserved_node *node, const core_binding *binding)
{
    const core_channel *channel = &binding->channel;
    if (!incident(channel, node->logical_node_id, node->instance)) return;
    core_channel_notification *chosen = NULL;
    for (size_t j = 0; j < CORE_MAX_REQUEST_CHANNELS; ++j) {
        core_channel_notification *event = &node->notifications[j];
        if (notification_matches(event, channel)) { chosen = event; break; }
        if (!event->occupied && chosen == NULL) chosen = event;
    }
    /* Capacity was checked before admission's publication boundary. */
    chosen->channel = (core_channel_id){channel->source_logical,
                                        channel->destination_logical, channel->service};
    chosen->creating_op = binding->creating_op;
    chosen->permissions = 0;
    chosen->attached = true;
    chosen->occupied = true;
}

static void reserve_notifications(core_reserved_node *node, const core_request_scope *scope)
{
    for (size_t i = 0; i < scope->channel_count; ++i)
        reserve_notification(node, &scope->bindings[i]);
}

static void detach_notification(core_admission *pool, const core_reserved_endpoint *endpoint)
{
    const core_instance_id instance = endpoint->sender ? endpoint->binding.channel.source_instance :
                                                       endpoint->binding.channel.destination_instance;
    core_reserved_node *node = find_node(pool, instance);
    if (node == NULL) return;
    for (size_t i = 0; i < CORE_MAX_REQUEST_CHANNELS; ++i) {
        core_channel_notification *event = &node->notifications[i];
        if (notification_matches(event, &endpoint->binding.channel) &&
            event->creating_op.value == endpoint->binding.creating_op.value) {
            event->attached = false;
            if (!event->pending) *event = (core_channel_notification){0};
            return;
        }
    }
}

core_admit_result core_admission_reserve(core_admission *pool, core_op_id id,
                                         const core_node_request *request,
                                         const core_request_scope *scope,
                                         uint64_t now_us, bool endpoint_only, bool cancel,
                                         const core_admission_record **out)
{
    uint64_t deadline, unused;
    if (!core_deadline(now_us, scope->allowance_us, &deadline) ||
        (!removal(scope->kind) &&
         !core_deadline(deadline, scope->cleanup_allowance_us, &unused))) {
        return CORE_ADMIT_INVALID;
    }
    core_admission_record *record = select_record(pool);
    if (record == NULL) {
        return CORE_ADMIT_FULL;
    }
    core_reserved_node *new_node = NULL;
    size_t bytes = 0;
    if (node_add(scope->kind) && !endpoint_only) {
        if (request->required_bytes > pool->node_bytes) {
            return CORE_ADMIT_INVALID;
        }
        if (node_fenced(pool, scope->logical_node_id, scope->instance)) {
            return CORE_ADMIT_CONFLICT;
        }
        for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
            if (pool->nodes[i].occupied && pool->nodes[i].instance.value == id.value) {
                return CORE_ADMIT_CONFLICT;
            }
        }
        new_node = free_node(pool);
        if (new_node == NULL) {
            return CORE_ADMIT_FULL;
        }
        bytes = pool->node_bytes;
    }
    if (node_remove(scope->kind)) {
        const core_reserved_node *target = find_node(pool, scope->instance);
        if (target != NULL && target->logical_node_id != scope->logical_node_id) {
            return CORE_ADMIT_CONFLICT;
        }
        const core_binding *bindings[CORE_MAX_REQUEST_CHANNELS];
        size_t count;
        if (!current_incidence(pool, scope->logical_node_id, scope->instance, bindings, &count))
            return CORE_ADMIT_CONFLICT;
        for (size_t i = 0; i < count; ++i)
            if (!scope_contains(scope, (core_channel_set)((1u << scope->channel_count) - 1u),
                                bindings[i])) return CORE_ADMIT_CONFLICT;
    }
    size_t endpoint_count = 0;
    for (size_t i = 0; i < scope->channel_count; ++i) {
        const core_binding *binding = &scope->bindings[i];
        const core_channel *channel = &binding->channel;
        if (!configured_owner(pool, channel->source_owner) ||
            !configured_owner(pool, channel->destination_owner) ||
            (!endpoint_only && channel->source_owner != pool->local_device &&
             channel->destination_owner != pool->local_device)) {
            return CORE_ADMIT_UNAUTHORIZED;
        }
        const core_reserved_node *known_source = find_node(pool, channel->source_instance);
        const core_reserved_node *known_destination = find_node(pool, channel->destination_instance);
        if ((known_source != NULL &&
             (known_source->logical_node_id != channel->source_logical ||
              channel->source_owner != pool->local_device)) ||
            (known_destination != NULL &&
             (known_destination->logical_node_id != channel->destination_logical ||
              channel->destination_owner != pool->local_device))) {
            return CORE_ADMIT_CONFLICT;
        }
        if (node_add(scope->kind) || node_remove(scope->kind)) {
            if (!incident(channel, scope->logical_node_id, scope->instance) ||
                (channel->source_instance.value == scope->instance.value &&
                 channel->source_owner != scope->coordinator) ||
                (channel->destination_instance.value == scope->instance.value &&
                 channel->destination_owner != scope->coordinator)) {
                return CORE_ADMIT_INVALID;
            }
        }
        for (size_t j = 0; j < i; ++j) {
            if (same_channel(channel, &scope->bindings[j].channel)) {
                return CORE_ADMIT_INVALID;
            }
        }
        /* Reclaimed endpoints may still have a retained creating/removal
         * scope. A fresh request cannot redefine that concrete binding. */
        for (size_t r = 0; r < CORE_MAX_ADMISSION_RECORDS; ++r) {
            const core_admission_record *known = &pool->records[r];
            if (!known->occupied) continue;
            for (size_t c = 0; c < known->scope.channel_count; ++c) {
                if (!(known->channel_known & (1u << c))) continue;
                const core_binding *previous = &known->scope.bindings[c];
                if (same_binding(binding, previous) &&
                    !same_descriptor(channel, &previous->channel)) return CORE_ADMIT_CONFLICT;
            }
        }
        if (!removal(scope->kind) && !cancel) {
            if (!core_deadline(now_us, channel->addition_allowance_us, &unused) ||
                !core_deadline(unused, scope->cleanup_allowance_us, &unused) ||
                (node_add(scope->kind) && channel->addition_allowance_us < scope->allowance_us)) {
                return CORE_ADMIT_INVALID;
            }
            if (binding_fenced(pool, binding) ||
                node_fenced(pool, channel->source_logical, channel->source_instance) ||
                node_fenced(pool, channel->destination_logical, channel->destination_instance) ||
                !local_port_matches(pool, scope, channel, true) ||
                !local_port_matches(pool, scope, channel, false)) {
                return CORE_ADMIT_CONFLICT;
            }
            endpoint_count += channel->source_owner == pool->local_device;
            endpoint_count += channel->destination_owner == pool->local_device;
        }
        for (size_t j = 0; j < CORE_MAX_ENDPOINT_RESERVATIONS; ++j) {
            const core_reserved_endpoint *endpoint = &pool->endpoints[j];
            if (!endpoint->occupied) {
                continue;
            }
            if (!removal(scope->kind) && !cancel && same_channel(channel, &endpoint->binding.channel)) {
                return CORE_ADMIT_CONFLICT;
            }
            if ((removal(scope->kind) || cancel) && same_binding(binding, &endpoint->binding) &&
                !same_descriptor(channel, &endpoint->binding.channel)) {
                return CORE_ADMIT_CONFLICT;
            }
        }
    }
    if (!removal(scope->kind) && !cancel) {
        for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
            const core_reserved_node *node = &pool->nodes[i];
            if (node->occupied &&
                (!incidence_fits(pool, scope, node->logical_node_id, node->instance) ||
                 !notifications_fit(node, scope))) {
                return CORE_ADMIT_FULL;
            }
        }
        if (new_node != NULL &&
            !incidence_fits(pool, scope, scope->logical_node_id, scope->instance)) {
            return CORE_ADMIT_FULL;
        }
    }
    core_reserved_endpoint *available[CORE_MAX_ENDPOINT_RESERVATIONS];
    size_t available_count = 0;
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        core_reserved_endpoint *endpoint = &pool->endpoints[i];
        if (!endpoint->occupied && endpoint->prepare.phase == CORE_ACTION_EMPTY &&
            endpoint->lifetime.last_ticket != UINT64_MAX &&
            core_lifetime_reclaimed(&endpoint->lifetime)) {
            available[available_count++] = endpoint;
        }
    }
    if (endpoint_count > available_count ||
        bytes > pool->byte_limit - pool->reserved_bytes ||
        endpoint_count > (pool->byte_limit - pool->reserved_bytes - bytes) / CORE_ENDPOINT_BYTES) {
        return CORE_ADMIT_FULL;
    }
    bytes += endpoint_count * CORE_ENDPOINT_BYTES;
    if (endpoint_only) {
        bool local_scope = false;
        for (size_t i = 0; i < scope->channel_count; ++i) {
            local_scope |= scope->bindings[i].channel.source_owner == pool->local_device ||
                           scope->bindings[i].channel.destination_owner == pool->local_device;
        }
        if (!local_scope) {
            return CORE_ADMIT_UNAUTHORIZED;
        }
    }
    core_report evidence;
    if (!core_report_init(&evidence, scope->kind, id, scope->coordinator, scope->object_count)) {
        return CORE_ADMIT_INVALID;
    }

    /* Every fallible validation, slot choice and budget check precedes this
     * publication. Stable lifetimes are reopened without resetting tickets. */
    publish_record(pool, record);
    if (new_node != NULL) {
        (void)core_lifetime_reopen(&new_node->lifetime);
        new_node->instance = scope->instance;
        new_node->logical_node_id = scope->logical_node_id;
        memcpy(new_node->ports, scope->ports, sizeof(new_node->ports));
        new_node->port_count = scope->port_count;
        memset(new_node->notifications, 0, sizeof(new_node->notifications));
        new_node->established = false;
        new_node->occupied = true;
    }
    size_t next = 0;
    if (!removal(scope->kind) && !cancel) {
        for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i)
            if (pool->nodes[i].occupied) reserve_notifications(&pool->nodes[i], scope);
        for (size_t i = 0; i < scope->channel_count; ++i) {
            const core_binding *binding = &scope->bindings[i];
            for (unsigned role = 0; role < 2; ++role) {
                const uint64_t owner = role == 0 ? binding->channel.source_owner :
                                                 binding->channel.destination_owner;
                if (owner != pool->local_device) {
                    continue;
                }
                core_reserved_endpoint *endpoint = available[next++];
                (void)core_lifetime_reopen(&endpoint->lifetime);
                endpoint->binding = *binding;
                endpoint->sender = role == 0;
                endpoint->occupied = true;
            }
        }
    }
    /* All checks have passed; initialize the free record in place rather than
     * constructing another full request/report record on the target stack. */
    record->id = id;
    record->deadline_us = endpoint_only ? 0 : deadline;
    record->node = new_node;
    record->evidence = evidence;
    record->request = *request;
    record->scope = *scope;
    record->channel_known = (core_channel_set)((1u << scope->channel_count) - 1u);
    record->channel_cleanup = cancel || removal(scope->kind) ? record->channel_known : 0;
    record->request_present = node_add(scope->kind) && !endpoint_only;
    record->cleanup_selected = cancel || removal(scope->kind);
    record->endpoint_only = endpoint_only;
    record->occupied = true;
    reserve_bytes(pool, bytes);
    *out = record;
    return CORE_ADMIT_ACCEPTED;
}

/* Provisional slot choices; only publication occupies them. */
typedef struct {
    core_admission_record *record;
    core_reserved_node *node;
    core_reserved_endpoint *endpoint;
    bool new_record;
} channel_reservation_plan;

static core_admit_result check_channel_command_owners(const core_admission *pool,
    uint64_t coordinator, core_op_id id, uint8_t index, const core_channel *channel)
{
    const bool new_source = channel->source_instance.value == id.value;
    const bool new_destination = channel->destination_instance.value == id.value;
    if (index >= CORE_MAX_REQUEST_CHANNELS || new_source == new_destination)
        return CORE_ADMIT_INVALID;
    const uint64_t new_owner = new_source ? channel->source_owner : channel->destination_owner;
    const uint64_t local_owner = new_source ? channel->destination_owner : channel->source_owner;
    if (!pool->configured || coordinator == pool->local_device || new_owner != coordinator ||
        local_owner != pool->local_device || !configured_owner(pool, coordinator))
        return CORE_ADMIT_UNAUTHORIZED;
    return CORE_ADMIT_ACCEPTED;
}

static core_admit_result check_retained_channel_binding(const core_admission_record *record,
    uint64_t coordinator, const core_binding *binding, uint8_t index)
{
    const core_op_id id = binding->creating_op;
    const core_channel *channel = &binding->channel;
    const uint64_t new_logical = channel->source_instance.value == id.value ?
        channel->source_logical : channel->destination_logical;
    const core_channel_set bit = (core_channel_set)(1u << index);
    if (!record->endpoint_only || record->scope.kind != CORE_REQUEST_ADD_NODE ||
        record->scope.coordinator != coordinator || record->scope.instance.value != id.value ||
        record->scope.logical_node_id != new_logical || index >= record->scope.channel_count)
        return CORE_ADMIT_CONFLICT;
    if (record->channel_known & bit) {
        if (!same_descriptor(&record->scope.bindings[index].channel, channel))
            return CORE_ADMIT_CONFLICT;
        return CORE_ADMIT_DUPLICATE;
    }
    return CORE_ADMIT_ACCEPTED;
}

static core_admit_result check_channel_binding_conflicts(const core_admission *pool,
    const core_admission_record *record, const core_binding *binding)
{
    const core_channel *channel = &binding->channel;
    for (size_t r = 0; r < CORE_MAX_ADMISSION_RECORDS; ++r) {
        const core_admission_record *known = &pool->records[r];
        if (!known->occupied) continue;
        for (size_t c = 0; c < known->scope.channel_count; ++c) {
            if (!(known->channel_known & (1u << c))) continue;
            const core_binding *previous = &known->scope.bindings[c];
            if (same_binding(binding, previous) &&
                (!same_descriptor(channel, &previous->channel) || known == record))
                return CORE_ADMIT_CONFLICT;
        }
    }
    return CORE_ADMIT_ACCEPTED;
}

static core_admit_result check_endpoint_conflicts_and_select_slot(core_admission *pool,
    const core_binding *binding, bool cancel, core_reserved_endpoint **out)
{
    const core_channel *channel = &binding->channel;
    core_reserved_endpoint *available = NULL;
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e) {
        core_reserved_endpoint *endpoint = &pool->endpoints[e];
        if (endpoint->occupied) {
            if ((cancel && same_binding(binding, &endpoint->binding) &&
                 !same_descriptor(channel, &endpoint->binding.channel)) ||
                (!cancel && same_channel(channel, &endpoint->binding.channel)))
                return CORE_ADMIT_CONFLICT;
        } else if (available == NULL && endpoint->prepare.phase == CORE_ACTION_EMPTY &&
                   endpoint->lifetime.last_ticket != UINT64_MAX &&
                   core_lifetime_reclaimed(&endpoint->lifetime)) {
            available = endpoint;
        }
    }
    *out = available;
    return CORE_ADMIT_ACCEPTED;
}

static core_admit_result check_local_endpoint_requirements(core_admission *pool,
    const core_binding *binding, core_reserved_node **out)
{
    const core_op_id id = binding->creating_op;
    const core_channel *channel = &binding->channel;
    const bool new_source = channel->source_instance.value == id.value;
    const uint64_t new_logical = new_source ? channel->source_logical : channel->destination_logical;
    const core_instance_id local_instance = new_source ? channel->destination_instance :
                                                         channel->source_instance;
    const uint64_t local_logical = new_source ? channel->destination_logical : channel->source_logical;
    const uint16_t local_port = new_source ? channel->destination_port : channel->source_port;
    core_reserved_node *local_node = find_node(pool, local_instance);
    if (binding_fenced(pool, binding) || find_node(pool, (core_instance_id){id.value}) != NULL ||
        node_fenced(pool, new_logical, (core_instance_id){id.value}) ||
        node_fenced(pool, local_logical, local_instance) ||
        local_node == NULL || local_node->logical_node_id != local_logical ||
        !local_node->established || local_node->lifetime.closing ||
        !port_matches(local_node->ports, local_node->port_count, local_port,
            new_source ? CORE_PORT_INPUT : CORE_PORT_OUTPUT, channel->type, channel->payload_bytes))
        return CORE_ADMIT_CONFLICT;
    *out = local_node;
    return CORE_ADMIT_ACCEPTED;
}

static core_admit_result check_node_channel_capacity(const core_admission *pool,
    const core_reserved_node *local_node, const core_channel *channel)
{
    const core_binding *bindings[CORE_MAX_REQUEST_CHANNELS];
    size_t incidence;
    if (!current_incidence(pool, local_node->logical_node_id, local_node->instance, bindings, &incidence))
        return CORE_ADMIT_FULL;
    bool notification_available = false;
    for (size_t n = 0; n < CORE_MAX_REQUEST_CHANNELS; ++n)
        notification_available |= !local_node->notifications[n].occupied ||
            notification_matches(&local_node->notifications[n], channel);
    return incidence >= CORE_MAX_REQUEST_CHANNELS || !notification_available ?
        CORE_ADMIT_FULL : CORE_ADMIT_ACCEPTED;
}

static void publish_channel_reservation(core_admission *pool,
    const channel_reservation_plan *plan, uint64_t coordinator,
    const core_binding *binding, uint8_t index, bool cancel)
{
    core_admission_record *record = plan->record;
    core_reserved_endpoint *available = plan->endpoint;
    const core_op_id id = binding->creating_op;
    const bool new_source = binding->channel.source_instance.value == id.value;
    const uint64_t new_logical = new_source ? binding->channel.source_logical :
                                             binding->channel.destination_logical;
    const core_channel_set bit = (core_channel_set)(1u << index);
    /* No fallible operation follows this publication boundary. A compact Ki
     * record has a bounded sparse scope; each learned descriptor is immutable.
     * Only Kc retains P and produces an aggregate result. */
    if (plan->new_record) {
        publish_record(pool, record);
        record->id = id;
        record->scope.kind = CORE_REQUEST_ADD_NODE;
        record->scope.coordinator = coordinator;
        record->scope.logical_node_id = new_logical;
        record->scope.instance = (core_instance_id){id.value};
        record->scope.channel_count = CORE_MAX_REQUEST_CHANNELS;
        record->scope.object_count = CORE_REPORT_MAX_OBJECTS;
        (void)core_report_init(&record->evidence, CORE_REQUEST_ADD_NODE, id,
                               coordinator, CORE_REPORT_MAX_OBJECTS);
        record->endpoint_only = true;
        record->occupied = true;
    }
    record->scope.bindings[index] = *binding;
    record->channel_known |= bit;
    if (cancel) {
        (void)core_admission_close_channel(pool, id, index);
    } else {
        (void)core_lifetime_reopen(&available->lifetime);
        available->binding = *binding;
        available->sender = !new_source;
        available->occupied = true;
        reserve_notification(plan->node, binding);
        reserve_bytes(pool, CORE_ENDPOINT_BYTES);
    }
}

core_admit_result core_admission_reserve_channel(core_admission *pool, uint64_t coordinator,
                                                 core_op_id id, uint8_t index,
                                                 const core_channel *channel, bool cancel,
                                                 const core_admission_record **out)
{
    core_admit_result result = check_channel_command_owners(pool, coordinator, id, index, channel);
    if (result != CORE_ADMIT_ACCEPTED) return result;
    const core_binding binding = {.channel = *channel, .creating_op = id};
    const core_admission_record *found = core_admission_find(pool, id);
    channel_reservation_plan plan = {
        .record = found == NULL ? select_record(pool) : &pool->records[found - pool->records],
        .new_record = found == NULL
    };
    if (found != NULL) {
        result = check_retained_channel_binding(found, coordinator, &binding, index);
        if (result == CORE_ADMIT_DUPLICATE) {
            if (cancel) (void)core_admission_close_channel(pool, id, index);
            *out = plan.record;
            return result;
        }
        if (result != CORE_ADMIT_ACCEPTED) return result;
        /* Whole-scope cancellation fences an unknown channel too. Learning
         * its descriptor must not reserve an endpoint. */
        cancel |= found->cleanup_selected;
    }
    if (plan.record == NULL) return CORE_ADMIT_FULL;
    result = check_channel_binding_conflicts(pool, plan.record, &binding);
    if (result != CORE_ADMIT_ACCEPTED) return result;
    result = check_endpoint_conflicts_and_select_slot(pool, &binding, cancel, &plan.endpoint);
    if (result != CORE_ADMIT_ACCEPTED) return result;
    if (!cancel) {
        result = check_local_endpoint_requirements(pool, &binding, &plan.node);
        if (result != CORE_ADMIT_ACCEPTED) return result;
        result = check_node_channel_capacity(pool, plan.node, channel);
        if (result != CORE_ADMIT_ACCEPTED) return result;
        if (plan.endpoint == NULL || CORE_ENDPOINT_BYTES > pool->byte_limit - pool->reserved_bytes)
            return CORE_ADMIT_FULL;
    }
    publish_channel_reservation(pool, &plan, coordinator, &binding, index, cancel);
    *out = plan.record;
    return CORE_ADMIT_ACCEPTED;
}

static core_admit_result check_removal_binding_owners(core_admission *pool,
    uint64_t coordinator, core_op_id id, uint8_t index, core_instance_id target,
    const core_binding *binding)
{
    const core_channel *channel = &binding->channel;
    const bool target_source = channel->source_instance.value == target.value;
    const bool target_destination = channel->destination_instance.value == target.value;
    if (index >= CORE_MAX_REQUEST_CHANNELS || target_source == target_destination ||
        id.value == target.value || id.value == binding->creating_op.value)
        return CORE_ADMIT_INVALID;
    const uint64_t target_owner = target_source ? channel->source_owner : channel->destination_owner;
    const uint64_t local_owner = target_source ? channel->destination_owner : channel->source_owner;
    if (!pool->configured || coordinator == pool->local_device || target_owner != coordinator ||
        local_owner != pool->local_device || !configured_owner(pool, coordinator))
        return CORE_ADMIT_UNAUTHORIZED;
    const core_reserved_node *source = find_node(pool, channel->source_instance);
    const core_reserved_node *destination = find_node(pool, channel->destination_instance);
    if ((source != NULL && (source->logical_node_id != channel->source_logical ||
                           channel->source_owner != pool->local_device)) ||
        (destination != NULL && (destination->logical_node_id != channel->destination_logical ||
                                channel->destination_owner != pool->local_device)))
        return CORE_ADMIT_CONFLICT;

    return CORE_ADMIT_ACCEPTED;
}

static core_admit_result check_retained_removal_binding(const core_admission_record *record,
    uint64_t coordinator, core_instance_id target, const core_binding *binding, uint8_t index)
{
    const core_channel *channel = &binding->channel;
    const uint64_t logical = channel->source_instance.value == target.value ?
        channel->source_logical : channel->destination_logical;
    const core_channel_set bit = (core_channel_set)(1u << index);
    if (!record->endpoint_only || record->scope.kind != CORE_REQUEST_REMOVE_NODE ||
        record->scope.coordinator != coordinator || record->scope.instance.value != target.value ||
        record->scope.logical_node_id != logical || index >= record->scope.channel_count)
        return CORE_ADMIT_CONFLICT;
    if (record->channel_known & bit) {
        if (!same_binding(&record->scope.bindings[index], binding) ||
            !same_descriptor(&record->scope.bindings[index].channel, channel))
            return CORE_ADMIT_CONFLICT;
        return CORE_ADMIT_DUPLICATE;
    }
    return CORE_ADMIT_ACCEPTED;
}

static core_admit_result check_removal_binding_conflicts(const core_admission *pool,
    const core_admission_record *record, const core_binding *binding)
{
    const core_channel *channel = &binding->channel;
    for (size_t r = 0; r < CORE_MAX_ADMISSION_RECORDS; ++r) {
        const core_admission_record *known = &pool->records[r];
        if (!known->occupied) continue;
        for (size_t c = 0; c < known->scope.channel_count; ++c) {
            if (!(known->channel_known & (1u << c))) continue;
            const core_binding *previous = &known->scope.bindings[c];
            if ((known == record && same_channel(channel, &previous->channel)) ||
                (same_binding(binding, previous) && !same_descriptor(channel, &previous->channel)))
                return CORE_ADMIT_CONFLICT;
        }
    }
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e) {
        const core_reserved_endpoint *endpoint = &pool->endpoints[e];
        if (endpoint->occupied && same_binding(binding, &endpoint->binding) &&
            !same_descriptor(channel, &endpoint->binding.channel)) return CORE_ADMIT_CONFLICT;
    }

    return CORE_ADMIT_ACCEPTED;
}

static void publish_removal_observer(core_admission *pool, core_admission_record *record,
    bool new_record, uint64_t coordinator, core_op_id id, uint8_t index,
    core_instance_id target, const core_binding *binding)
{
    const core_channel *channel = &binding->channel;
    const uint64_t logical = channel->source_instance.value == target.value ?
        channel->source_logical : channel->destination_logical;
    const core_channel_set bit = (core_channel_set)(1u << index);
    /* All checks precede publication. One sparse observer holds the removal
     * identity; endpoint lifetime and payload ownership remain shared. */
    if (new_record) {
        publish_record(pool, record);
        record->id = id;
        record->scope.kind = CORE_REQUEST_REMOVE_NODE;
        record->scope.coordinator = coordinator;
        record->scope.logical_node_id = logical;
        record->scope.instance = target;
        record->scope.channel_count = CORE_MAX_REQUEST_CHANNELS;
        record->scope.object_count = CORE_REPORT_MAX_OBJECTS;
        (void)core_report_init(&record->evidence, CORE_REQUEST_REMOVE_NODE, id,
                               coordinator, CORE_REPORT_MAX_OBJECTS);
        record->cleanup_selected = true;
        record->endpoint_only = true;
        record->occupied = true;
    }
    record->scope.bindings[index] = *binding;
    record->channel_known |= bit;
    (void)core_admission_close_channel(pool, id, index);
}

core_admit_result core_admission_reserve_removal(core_admission *pool, uint64_t coordinator,
                                                 core_op_id id, uint8_t index,
                                                 core_instance_id target,
                                                 const core_binding *binding,
                                                 const core_admission_record **out)
{
    core_admit_result result = check_removal_binding_owners(pool, coordinator, id, index,
                                                           target, binding);
    if (result != CORE_ADMIT_ACCEPTED) return result;
    const core_admission_record *found = core_admission_find(pool, id);
    core_admission_record *record = found == NULL ? select_record(pool) :
        &pool->records[found - pool->records];
    if (found != NULL) {
        result = check_retained_removal_binding(found, coordinator, target, binding, index);
        if (result == CORE_ADMIT_DUPLICATE) {
            (void)core_admission_close_channel(pool, id, index);
            *out = record;
            return result;
        }
        if (result != CORE_ADMIT_ACCEPTED) return result;
    }
    if (record == NULL) return CORE_ADMIT_FULL;
    result = check_removal_binding_conflicts(pool, record, binding);
    if (result != CORE_ADMIT_ACCEPTED) return result;
    publish_removal_observer(pool, record, found == NULL, coordinator, id, index, target, binding);
    *out = record;
    return CORE_ADMIT_ACCEPTED;
}

bool core_admission_establish_node(core_admission *pool, core_instance_id instance)
{
    core_reserved_node *node = find_node(pool, instance);
    if (node == NULL || node->lifetime.closing ||
        node_fenced(pool, node->logical_node_id, instance)) {
        return false;
    }
    node->established = true;
    return true;
}

bool core_admission_close_scope(core_admission *pool, core_op_id id)
{
    const core_admission_record *found = core_admission_find(pool, id);
    if (found == NULL) {
        return false;
    }
    core_admission_record *record = &pool->records[found - pool->records];
    record->cleanup_selected = true;
    record->channel_cleanup |= record->channel_known;
    if (!record->endpoint_only && (node_add(record->scope.kind) || node_remove(record->scope.kind))) {
        core_reserved_node *node = find_node(pool, record->scope.instance);
        if (node != NULL && node->logical_node_id == record->scope.logical_node_id) {
            core_lifetime_close(&node->lifetime);
            node->established = false;
        }
    } else if (record->scope.kind == 0 && record->node != NULL) {
        core_lifetime_close(&record->node->lifetime);
    }
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        core_reserved_endpoint *endpoint = &pool->endpoints[i];
        if (endpoint->occupied && scope_contains(&record->scope, record->channel_known,
                                                 &endpoint->binding)) {
            core_lifetime_close(&endpoint->lifetime);
        }
    }
    return true;
}

bool core_admission_close_channel(core_admission *pool, core_op_id id, uint8_t channel)
{
    const core_admission_record *found = core_admission_find(pool, id);
    if (found == NULL || channel >= found->scope.channel_count ||
        !(found->channel_known & (1u << channel))) return false;
    core_admission_record *record = &pool->records[found - pool->records];
    record->channel_cleanup |= (core_channel_set)(1u << channel);
    const core_binding *binding = &record->scope.bindings[channel];
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        core_reserved_endpoint *endpoint = &pool->endpoints[i];
        if (endpoint->occupied && same_binding(&endpoint->binding, binding) &&
            same_descriptor(&endpoint->binding.channel, &binding->channel))
            core_lifetime_close(&endpoint->lifetime);
    }
    return true;
}

bool core_admission_release_endpoints(core_admission *pool, core_op_id creating_id)
{
    bool found = false;
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        const core_reserved_endpoint *endpoint = &pool->endpoints[i];
        if (endpoint->occupied && endpoint->binding.creating_op.value == creating_id.value) {
            found = true;
            if (!core_lifetime_reclaimed(&endpoint->lifetime) || endpoint->prepare.phase != CORE_ACTION_EMPTY) {
                return false;
            }
        }
    }
    if (!found) {
        return false;
    }
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        core_reserved_endpoint *endpoint = &pool->endpoints[i];
        if (endpoint->occupied && endpoint->binding.creating_op.value == creating_id.value) {
            detach_notification(pool, endpoint);
            endpoint->occupied = false;
            pool->reserved_bytes -= CORE_ENDPOINT_BYTES;
        }
    }
    return true;
}

bool core_admission_release_binding(core_admission *pool, const core_binding *binding)
{
    bool found = false;
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        const core_reserved_endpoint *endpoint = &pool->endpoints[i];
        if (endpoint->occupied && same_binding(&endpoint->binding, binding)) {
            found = true;
            if (!same_descriptor(&endpoint->binding.channel, &binding->channel) ||
                !core_lifetime_reclaimed(&endpoint->lifetime) ||
                endpoint->prepare.phase != CORE_ACTION_EMPTY) {
                return false;
            }
        }
    }
    if (!found) {
        return false;
    }
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        core_reserved_endpoint *endpoint = &pool->endpoints[i];
        if (endpoint->occupied && same_binding(&endpoint->binding, binding)) {
            detach_notification(pool, endpoint);
            endpoint->occupied = false;
            pool->reserved_bytes -= CORE_ENDPOINT_BYTES;
        }
    }
    return true;
}

bool core_admission_release_node(core_admission *pool, core_op_id id)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *record = &pool->records[i];
        if (!record->occupied || record->id.value != id.value) {
            continue;
        }
        core_reserved_node *node = record->node;
        if (node == NULL || !core_lifetime_reclaimed(&node->lifetime) ||
            node->prepare.phase != CORE_ACTION_EMPTY) {
            return false;
        }
        for (size_t j = 0; j < CORE_MAX_ENDPOINT_RESERVATIONS; ++j) {
            const core_reserved_endpoint *endpoint = &pool->endpoints[j];
            if (endpoint->occupied && incident(&endpoint->binding.channel, node->logical_node_id,
                                              node->instance)) {
                return false;
            }
        }
        node->occupied = false;
        memset(node->notifications, 0, sizeof(node->notifications));
        record->node = NULL;
        pool->reserved_bytes -= pool->node_bytes;
        return true;
    }
    return false;
}

static bool resources_released(const core_admission *pool, const core_admission_record *record)
{
    if (record->node != NULL) return false;
    for (size_t j = 0; j < CORE_MAX_ENDPOINT_RESERVATIONS; ++j)
        if (pool->endpoints[j].occupied && scope_contains(&record->scope,
                record->channel_known, &pool->endpoints[j].binding)) return false;
    if (!record->endpoint_only && node_remove(record->scope.kind))
        for (size_t n = 0; n < CORE_MAX_NODE_RESERVATIONS; ++n)
            if (pool->nodes[n].occupied &&
                pool->nodes[n].instance.value == record->scope.instance.value) return false;
    return true;
}

static bool reclaimable(const core_admission *pool, const core_admission_record *r)
{
    if (!r->occupied || r->scope.kind == 0 || !resources_released(pool, r)) return false;
    if (!r->endpoint_only) {
        const core_outcome outcome = core_report_outcome(&r->evidence);
        if ((outcome != CORE_OUTCOME_SUCCEEDED && outcome != CORE_OUTCOME_FAILED_CLEAN) ||
            core_report_obligations(&r->evidence) != 0) return false;
        if ((node_add(r->scope.kind) || node_remove(r->scope.kind)) &&
            !(r->evidence.objects[0] & CORE_OBJECT_CLEANED)) return false;
    }
    for (size_t c = 0; c < r->scope.channel_count; ++c) {
        if (!(r->channel_known & (1u << c))) continue;
        for (unsigned role = 0; role < 2; ++role) {
            const core_binding *binding = &r->scope.bindings[c];
            const uint64_t owner = role == 0 ? binding->channel.source_owner :
                                              binding->channel.destination_owner;
            const size_t object = endpoint_object(r, c, role);
            if (r->endpoint_only) {
                if (owner == pool->local_device &&
                    (!(r->local_facts[object] & CORE_OBJECT_CLEANED) ||
                     !(r->owner_delivered & (UINT32_C(1) << object)))) return false;
            } else if (!(r->evidence.objects[object] & CORE_OBJECT_CLEANED) &&
                       !remote_closed(pool, binding, role)) return false;
        }
    }
    return true;
}

bool core_admission_report_retained(const core_admission_record *r)
{
    return r != NULL && r->occupied && !r->endpoint_only && r->scope.kind != 0 &&
        (!r->report_acknowledged || r->acknowledged_revision != r->evidence.revision);
}

void core_admission_acknowledge(core_admission *pool, core_op_id id,
                                uint64_t coordinator, uint8_t revision)
{
    const core_admission_record *found = core_admission_find(pool, id);
    if (found == NULL || found->endpoint_only || found->scope.kind == 0 ||
        found->evidence.coordinator != coordinator || found->evidence.revision != revision) return;
    core_admission_record *r = &pool->records[found - pool->records];
    r->report_acknowledged = true;
    r->acknowledged_revision = revision;
}

void core_admission_maintain(core_admission *pool)
{
    /* Rank first observations of retained versions; neither retries nor ACKs
     * make an unchanged snapshot younger. Ties use stable slot order. */
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *r = &pool->records[i];
        if (!r->occupied || (r->report_age != 0 && r->report_revision == r->evidence.revision))
            continue;
        uint8_t newest = 0;
        for (size_t j = 0; j < CORE_MAX_ADMISSION_RECORDS; ++j) {
            core_admission_record *other = &pool->records[j];
            if (!other->occupied || other == r) continue;
            if (r->report_age != 0 && other->report_age > r->report_age) --other->report_age;
            if (other->report_age > newest) newest = other->report_age;
        }
        r->report_age = (uint8_t)(newest + 1);
        r->report_revision = r->evidence.revision;
    }
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *r = &pool->records[i];
        /* Owner delivery is not an O/DB storage ACK. Keep its settled scope
         * as a rejection fence until successful admission needs the slot. */
        if (!r->endpoint_only && reclaimable(pool, r) && !core_admission_report_retained(r))
            (void)core_admission_forget(pool, r->id);
    }
}

/* Copy physical closure to every still-retained reference before discarding
 * its only observer. This bookkeeping never advances another report stream. */
static void preserve_closure(core_admission *pool, const core_admission_record *retiring)
{
    if (retiring->endpoint_only) return;
    for (size_t c = 0; c < retiring->scope.channel_count; ++c) {
        if (!(retiring->channel_known & (1u << c))) continue;
        for (unsigned role = 0; role < 2; ++role) {
            const size_t object = endpoint_object(retiring, c, role);
            if (!(retiring->evidence.objects[object] & CORE_OBJECT_CLEANED) &&
                !(retiring->closed_objects & (UINT32_C(1) << object))) continue;
            for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
                core_admission_record *r = &pool->records[i];
                if (!r->occupied || r->endpoint_only || r == retiring) continue;
                for (size_t j = 0; j < r->scope.channel_count; ++j)
                    if ((r->channel_known & (1u << j)) &&
                        same_binding(&retiring->scope.bindings[c], &r->scope.bindings[j]) &&
                        same_descriptor(&retiring->scope.bindings[c].channel,
                                        &r->scope.bindings[j].channel))
                        r->closed_objects |= UINT32_C(1) << endpoint_object(r, j, role);
            }
        }
    }
}

bool core_admission_forget(core_admission *pool, core_op_id id)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *record = &pool->records[i];
        if (!record->occupied || record->id.value != id.value) {
            continue;
        }
        if (!resources_released(pool, record)) return false;
        preserve_closure(pool, record);
        for (size_t j = 0; j < CORE_MAX_ADMISSION_RECORDS; ++j)
            if (record->report_age != 0 && pool->records[j].report_age > record->report_age)
                --pool->records[j].report_age;
        *record = (core_admission_record){.incarnation = record->incarnation};
        return true;
    }
    return false;
}

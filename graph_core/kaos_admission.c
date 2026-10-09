#include "kaos_admission_internal.h"

#include <string.h>

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
        if (!record->occupied || (!removal(record->scope.kind) && !record->cleanup_selected)) {
            continue;
        }
        for (size_t j = 0; j < record->scope.channel_count; ++j) {
            if (same_binding(&record->scope.bindings[j], binding)) {
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

static core_admission_record *free_record(core_admission *pool)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        if (!pool->records[i].occupied) {
            return &pool->records[i];
        }
    }
    return NULL;
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
    core_admission_record *record = free_record(pool);
    core_reserved_node *node = free_node(pool);
    if (pool->node_bytes > pool->byte_limit - pool->reserved_bytes || record == NULL || node == NULL) {
        return CORE_ADMIT_FULL;
    }
    (void)core_lifetime_reopen(&node->lifetime);
    node->instance = (core_instance_id){ .value = id.value };
    node->logical_node_id = 0;
    node->port_count = 0;
    node->established = false;
    node->occupied = true;
    *record = (core_admission_record){.id = id, .deadline_us = deadline, .node = node, .occupied = true};
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

static bool scope_contains(const core_request_scope *scope, const core_binding *binding)
{
    for (size_t i = 0; i < scope->channel_count; ++i) {
        if (same_binding(&scope->bindings[i], binding) &&
            same_descriptor(&scope->bindings[i].channel, &binding->channel)) {
            return true;
        }
    }
    return false;
}

/* Complete future node removal must fit the same bounded wire scope. Count
 * concrete bindings once even when both endpoint roles are local. Occupied
 * cleanup reservations still count until their local release. */
static bool incidence_fits(const core_admission *pool, const core_request_scope *scope,
                           uint64_t logical, core_instance_id instance)
{
    size_t count = 0;
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        const core_reserved_endpoint *endpoint = &pool->endpoints[i];
        if (!endpoint->occupied || !incident(&endpoint->binding.channel, logical, instance)) {
            continue;
        }
        bool seen = false;
        for (size_t j = 0; j < i; ++j) {
            seen |= pool->endpoints[j].occupied &&
                    same_binding(&pool->endpoints[j].binding, &endpoint->binding);
        }
        if (!seen) ++count;
    }
    /* Existing-channel conflicts and duplicate request channels were rejected
     * before this check, so each matching new descriptor adds one binding. */
    for (size_t i = 0; i < scope->channel_count; ++i) {
        if (incident(&scope->bindings[i].channel, logical, instance)) ++count;
    }
    return count <= CORE_MAX_REQUEST_CHANNELS;
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
    core_admission_record *record = free_record(pool);
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
        for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
            const core_reserved_endpoint *endpoint = &pool->endpoints[i];
            if (endpoint->occupied && incident(&endpoint->binding.channel, scope->logical_node_id,
                                              scope->instance) &&
                !scope_contains(scope, &endpoint->binding)) {
                return CORE_ADMIT_CONFLICT;
            }
        }
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
                !incidence_fits(pool, scope, node->logical_node_id, node->instance)) {
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
    if (new_node != NULL) {
        (void)core_lifetime_reopen(&new_node->lifetime);
        new_node->instance = scope->instance;
        new_node->logical_node_id = scope->logical_node_id;
        memcpy(new_node->ports, scope->ports, sizeof(new_node->ports));
        new_node->port_count = scope->port_count;
        new_node->established = false;
        new_node->occupied = true;
    }
    size_t next = 0;
    if (!removal(scope->kind) && !cancel) {
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
    memset(record, 0, sizeof(*record));
    record->id = id;
    record->deadline_us = endpoint_only ? 0 : deadline;
    record->node = new_node;
    record->evidence = evidence;
    record->request = *request;
    record->scope = *scope;
    record->request_present = node_add(scope->kind) && !endpoint_only;
    record->cleanup_selected = cancel || removal(scope->kind);
    record->endpoint_only = endpoint_only;
    record->occupied = true;
    reserve_bytes(pool, bytes);
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
        if (endpoint->occupied && scope_contains(&record->scope, &endpoint->binding)) {
            core_lifetime_close(&endpoint->lifetime);
        }
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
        record->node = NULL;
        pool->reserved_bytes -= pool->node_bytes;
        return true;
    }
    return false;
}

bool core_admission_forget(core_admission *pool, core_op_id id)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *record = &pool->records[i];
        if (!record->occupied || record->id.value != id.value) {
            continue;
        }
        if (record->node != NULL) {
            return false;
        }
        for (size_t j = 0; j < CORE_MAX_ENDPOINT_RESERVATIONS; ++j) {
            if (pool->endpoints[j].occupied &&
                scope_contains(&record->scope, &pool->endpoints[j].binding)) {
                return false;
            }
        }
        if (!record->endpoint_only && node_remove(record->scope.kind) &&
            find_node(pool, record->scope.instance) != NULL) {
            return false;
        }
        *record = (core_admission_record){0};
        return true;
    }
    return false;
}

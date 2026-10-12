#include "kaos_node.h"
#include "kaos_edge.h"

static core_admission_record *find(core_node_service *s, core_op_id id)
{
    const core_admission_record *r = core_admission_find(s->admission, id);
    return r == NULL ? NULL : &s->admission->records[r - s->admission->records];
}

bool core_node_supported(const core_admission_record *r)
{
    return r != NULL && r->occupied && !r->endpoint_only &&
        (r->scope.kind == CORE_REQUEST_ADD_NODE_NO_EDGES ||
         r->scope.kind == CORE_REQUEST_ADD_NODE ||
         r->scope.kind == CORE_REQUEST_REMOVE_NODE_NO_EDGES ||
         r->scope.kind == CORE_REQUEST_REMOVE_NODE);
}

static bool succeeded(const core_admission_record *r)
{
    return (r->evidence.history & CORE_HISTORY_SUCCESS) != 0;
}

static void observe(core_admission_record *r, uint8_t facts)
{
    (void)core_report_observe(&r->evidence, 0, r->evidence.objects[0] | facts);
}

/* The caller holds the affected runtime mailbox lock when a worker exists. */
static void select_cleanup(core_node_service *s, core_admission_record *r, uint64_t now)
{
    if (r->cleanup_deadline_us == 0) {
        if (succeeded(r)) {
            /* Local reclamation is not a newly authorized removal. */
            r->cleanup_deadline_us = UINT64_MAX;
        } else if (r->scope.kind == CORE_REQUEST_REMOVE_NODE_NO_EDGES ||
                   r->scope.kind == CORE_REQUEST_REMOVE_NODE) {
            r->cleanup_deadline_us = r->deadline_us;
        } else if (!core_deadline(now, r->scope.cleanup_allowance_us,
                                  &r->cleanup_deadline_us)) {
            r->cleanup_deadline_us = now;
        }
    }
    if (!succeeded(r)) (void)core_report_record(&r->evidence, CORE_HISTORY_CLEANUP);
    observe(r, CORE_OBJECT_CLEANUP);
    (void)core_admission_close_scope(s->admission, r->id);
}

static bool worker_only(const core_lifetime *owner, core_access worker)
{
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        if (owner->accesses[i] != 0 && owner->accesses[i] != worker.ticket) return false;
    }
    return true;
}

static void cleanup(core_node_service *s, core_admission_record *r, uint64_t now)
{
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        const core_reserved_node *node = &s->admission->nodes[i];
        const core_node_execution *e = &s->executions[i];
        if (!e->occupied || !node->occupied ||
            node->instance.value != r->scope.instance.value ||
            node->logical_node_id != r->scope.logical_node_id) continue;
        core_runtime_state *state = s->runtime.lock(s->runtime.context, i);
        select_cleanup(s, r, now);
        state->cancel_requested = true;
        if (worker_only(&node->lifetime, e->worker)) state->dispose_requested = true;
        s->runtime.unlock(s->runtime.context, i);
        return;
    }
    select_cleanup(s, r, now);
}

bool core_node_artifact(core_node_service *s, core_op_id id, uint8_t *artifact,
                        size_t size, uint64_t now)
{
    core_admission_record *r = find(s, id);
    if (!core_node_supported(r) || (r->scope.kind != CORE_REQUEST_ADD_NODE_NO_EDGES &&
         r->scope.kind != CORE_REQUEST_ADD_NODE) ||
        r->node == NULL || artifact == NULL || size != r->request.artifact_bytes ||
        r->cleanup_selected || r->node->lifetime.closing ||
        !core_before_deadline(now, r->deadline_us) || s->runtime.prepare == NULL ||
        s->runtime.lock == NULL || s->runtime.unlock == NULL) return false;
    core_reserved_node *node = r->node;
    const size_t index = (size_t)(node - s->admission->nodes);
    core_node_execution *e = &s->executions[index];
    const size_t arena = r->request.required_bytes - size;
    core_lifetime *owner = &node->lifetime;
    if (e->occupied || node->prepare.phase != CORE_ACTION_EMPTY ||
        !core_lifetime_unclaimed(owner) || owner->bytes != size ||
        owner->allocations != 1 || owner->last_ticket == UINT64_MAX ||
        arena > owner->byte_limit - owner->bytes) return false;

    /* Admission and these checks guarantee both reservations. Preparation
     * retains one ownership action through private runtime destruction. */
    if (arena != 0 && !core_lifetime_charge(owner, arena)) return false;
    core_access token;
    (void)core_action_issue(&node->prepare, owner, &token);
    (void)core_action_begin(&node->prepare, token);
    if (!s->runtime.prepare(s->runtime.context, index, artifact, size,
                            r->request.configuration, r->request.configuration_size, arena)) {
        core_work_result result;
        (void)core_action_complete(&node->prepare, token, CORE_WORK_FAILED);
        (void)core_action_accept(&node->prepare, token, &result);
        observe(r, CORE_OBJECT_FAILED);
        select_cleanup(s, r, now);
        if (arena != 0) (void)core_lifetime_uncharge(owner, arena);
        return false; /* caller still owns the charged artifact */
    }
    *e = (core_node_execution){.id = id, .worker = token,
        .artifact_bytes = size, .arena_bytes = arena, .occupied = true,
        .artifact_charged = true, .arena_charged = arena != 0};
    return true;
}

static core_channel_set node_channels(const core_node_service *s,
    const core_admission_record *r, uint8_t fact)
{
    core_channel_set channels = core_edge_channels(r, fact);
    for (size_t c = 0; c < r->scope.channel_count; ++c) {
        if (!(channels & (1u << c))) continue;
        const core_binding *binding = &r->scope.bindings[c];
        for (unsigned role = 0; role < 2; ++role) {
            const uint64_t owner = role == 0 ? binding->channel.source_owner :
                                              binding->channel.destination_owner;
            const core_instance_id instance = role == 0 ? binding->channel.source_instance :
                                                          binding->channel.destination_instance;
            if (owner != s->admission->local_device) continue;
            bool node_live = false, endpoint_live = false;
            for (size_t n = 0; n < CORE_MAX_NODE_RESERVATIONS; ++n) {
                const core_reserved_node *node = &s->admission->nodes[n];
                node_live |= node->occupied && node->instance.value == instance.value &&
                             !node->lifetime.closing;
            }
            for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
                const core_reserved_endpoint *endpoint = &s->admission->endpoints[i];
                endpoint_live |= endpoint->occupied && !endpoint->lifetime.closing &&
                    endpoint->sender == (role == 0) &&
                    endpoint->binding.creating_op.value == binding->creating_op.value &&
                    endpoint->binding.channel.source_logical == binding->channel.source_logical &&
                    endpoint->binding.channel.destination_logical == binding->channel.destination_logical &&
                    endpoint->binding.channel.service == binding->channel.service;
            }
            if (!node_live || !endpoint_live) channels &= (core_channel_set)~(1u << c);
        }
    }
    return channels;
}

static void execution_step(core_node_service *s, size_t index, uint64_t now)
{
    core_node_execution *e = &s->executions[index];
    if (!e->occupied) return;
    core_admission_record *r = find(s, e->id);
    if (r == NULL || r->node != &s->admission->nodes[index]) return;
    core_reserved_node *node = r->node;
    core_runtime_state *state = s->runtime.lock(s->runtime.context, index);

    /* Exit/liveness is observed before INIT, under the worker's same lock.
     * A marker emitted before an already-observed exit cannot start a node. */
    if (state->exited || state->prepare_failed) {
        if (state->exited) observe(r, CORE_OBJECT_EXITED);
        if (state->fault || state->prepare_failed) observe(r, CORE_OBJECT_FAILED);
        select_cleanup(s, r, now);
    }
    if (!r->cleanup_selected && !node->lifetime.closing && state->prepared) {
        observe(r, CORE_OBJECT_READY);
        if (!(r->evidence.history & CORE_HISTORY_COMMIT) &&
            core_predicate_eval(&r->scope.predicate, node_channels(s, r, CORE_OBJECT_READY)))
            (void)core_report_record(&r->evidence, CORE_HISTORY_COMMIT);
        if ((r->evidence.history & CORE_HISTORY_COMMIT) &&
            !(r->evidence.history & CORE_HISTORY_DISPATCH) &&
            core_predicate_eval(&r->scope.predicate, node_channels(s, r, CORE_OBJECT_ACTIVE))) {
            (void)core_report_record(&r->evidence, CORE_HISTORY_DISPATCH);
            state->start_deadline_us = r->deadline_us;
            state->start_requested = true;
        }
    }
    if (state->init_pending) {
        state->init_pending = false;
        if (!succeeded(r) && !r->cleanup_selected && !node->lifetime.closing &&
            state->main_active && !state->exited && state->prepared &&
            (r->evidence.history & CORE_HISTORY_DISPATCH) &&
            core_predicate_eval(&r->scope.predicate, node_channels(s, r, CORE_OBJECT_ACTIVE)) &&
            core_before_deadline(now, r->deadline_us)) {
            if (core_admission_establish_node(s->admission, node->instance)) {
                (void)core_report_record(&r->evidence,
                                         CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS);
            } else {
                select_cleanup(s, r, now);
            }
        }
    }
    /* Generic ERROR is a reporting-only placeholder, not runtime failure or
     * cleanup evidence. Its bounded FIFO is owned by the runtime adapter. */
    if (r->cleanup_selected || node->lifetime.closing) {
        select_cleanup(s, r, now);
        state->cancel_requested = true;
        if (worker_only(&node->lifetime, e->worker)) state->dispose_requested = true;
    }
    const bool disposed = state->disposed && !state->main_active && !state->configuration_active;
    s->runtime.unlock(s->runtime.context, index);
    if (!disposed || !r->cleanup_selected || !node->lifetime.closing) return;

    if (!e->worker_accepted) {
        core_work_result result;
        if (!core_action_complete(&node->prepare, e->worker, CORE_WORK_CANCELLED) ||
            !core_action_accept(&node->prepare, e->worker, &result)) return;
        e->worker_accepted = true;
    }
    if (!core_lifetime_unclaimed(&node->lifetime)) return;
    observe(r, CORE_OBJECT_STOPPED);
    if (e->artifact_charged) {
        if (!core_lifetime_uncharge(&node->lifetime, e->artifact_bytes)) return;
        e->artifact_charged = false;
    }
    if (e->arena_charged) {
        if (!core_lifetime_uncharge(&node->lifetime, e->arena_bytes)) return;
        e->arena_charged = false;
    }
    if (core_admission_release_node(s->admission, e->id)) {
        observe(r, CORE_OBJECT_CLEANED);
        *e = (core_node_execution){0};
    }
}

static bool node_present(const core_node_service *s, const core_request_scope *scope)
{
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        const core_reserved_node *node = &s->admission->nodes[i];
        if (node->occupied && node->instance.value == scope->instance.value &&
            node->logical_node_id == scope->logical_node_id) return true;
    }
    return false;
}

void core_node_step(core_node_service *s, uint64_t now)
{
    /* Admitted removals win before processing any queued startup evidence. */
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *r = &s->admission->records[i];
        if (core_node_supported(r) &&
            (r->scope.kind == CORE_REQUEST_REMOVE_NODE_NO_EDGES ||
             r->scope.kind == CORE_REQUEST_REMOVE_NODE))
            cleanup(s, r, now);
    }
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *r = &s->admission->records[i];
        if (!core_node_supported(r)) continue;
        if (r->cleanup_selected || (r->node != NULL && r->node->lifetime.closing)) {
            cleanup(s, r, now);
        } else if (!succeeded(r) && r->scope.kind == CORE_REQUEST_ADD_NODE &&
            !core_predicate_eval(&r->scope.predicate, node_channels(s, r,
                (r->evidence.history & CORE_HISTORY_DISPATCH) ? CORE_OBJECT_ACTIVE : 0))) {
            cleanup(s, r, now);
        } else if (!succeeded(r) && !core_before_deadline(now, r->deadline_us)) {
            observe(r, CORE_OBJECT_EXPIRED);
            cleanup(s, r, now);
        }
        if (r->cleanup_deadline_us != 0 && !succeeded(r) &&
            !core_before_deadline(now, r->cleanup_deadline_us) &&
            core_report_obligations(&r->evidence) != 0) {
            (void)core_report_record(&r->evidence, CORE_HISTORY_CUTOFF);
        }
    }
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) execution_step(s, i, now);
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *r = &s->admission->records[i];
        if (!core_node_supported(r) || !r->cleanup_selected) continue;
        /* No dispatched worker: the artifact adapter first cancels its
         * transfer, frees owned staging and releases that separate charge. */
        if (r->node != NULL &&
            !s->executions[r->node - s->admission->nodes].occupied &&
            core_lifetime_reclaimed(&r->node->lifetime)) {
            (void)core_admission_release_node(s->admission, r->id);
        }
        if (!node_present(s, &r->scope)) {
            observe(r, CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED);
        }
    }
}

uint64_t core_node_next_deadline(const core_node_service *s)
{
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        const core_admission_record *r = &s->admission->records[i];
        if (!core_node_supported(r) || succeeded(r)) continue;
        uint64_t candidate;
        if (r->cleanup_selected) {
            if (core_report_obligations(&r->evidence) == 0 ||
                (r->evidence.history & CORE_HISTORY_CUTOFF)) continue;
            /* The adapter calls step after admission/completion. Until then
             * request immediate control service to establish the deadline. */
            candidate = r->cleanup_deadline_us;
        } else {
            candidate = r->deadline_us;
        }
        if (candidate < next) next = candidate;
    }
    return next;
}

/* Remote health is represented only by accepted evidence. Locally owned
 * roles must also still exist and permit access at this decision boundary. */
bool core_node_start_allowed(const core_node_service *s, size_t slot, uint64_t now)
{
    if (slot >= CORE_MAX_NODE_RESERVATIONS || !s->executions[slot].occupied) return false;
    const core_admission_record *r = core_admission_find(s->admission, s->executions[slot].id);
    return core_node_supported(r) && r->node == &s->admission->nodes[slot] &&
        !r->cleanup_selected && !r->node->lifetime.closing &&
        (r->evidence.history & CORE_HISTORY_DISPATCH) &&
        core_before_deadline(now, r->deadline_us) &&
        core_predicate_eval(&r->scope.predicate, node_channels(s, r, CORE_OBJECT_ACTIVE));
}

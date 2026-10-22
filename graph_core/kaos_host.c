#include "kaos_host.h"
#include "kaos_wamr.h"

#include <stdlib.h>
#include <string.h>

static void lock(core_host *h)
{
    h->platform.lock(h->platform.context);
}

static void unlock(core_host *h)
{
    h->platform.unlock(h->platform.context);
}

static bool now(core_host *h, uint64_t *time)
{
    return h->platform.now(h->platform.context, time);
}

static void count(uint64_t *value)
{
    if (*value != UINT64_MAX)
        ++*value;
}

static void maximum(uint64_t *value, uint64_t sample)
{
    if (sample > *value)
        *value = sample;
}

static bool send_packet(core_host *h, core_host_class traffic_class, uint64_t peer,
                        const uint8_t *bytes, size_t size)
{
    const bool sent = h->platform.send(h->platform.context, traffic_class, peer, bytes, size);
    count(sent ? &h->stats.sent[traffic_class] : &h->stats.send_failures[traffic_class]);
    return sent;
}

static void wake(void *context)
{
    core_host *h = context;
    h->platform.wake(h->platform.context);
}

static void diagnostic(core_host *h, const char *name, core_op_id id, size_t value)
{
    if (h->platform.diagnostic != NULL)
        h->platform.diagnostic(h->platform.context, name, id, value);
}

static core_channel_id channel_id(const uint64_t id[3])
{
    return (core_channel_id){id[0], id[1], id[2]};
}

static int32_t channel_put(void *context, size_t slot, const uint64_t channel[3],
                           const uint8_t *data, size_t size)
{
    core_host *h = context;
    lock(h);
    const int32_t result = core_channel_put(&h->endpoints, h->admission.nodes[slot].instance,
                                            channel_id(channel), data, size);
    if (result == CORE_DATA_OK && h->platform.channel_observation != NULL)
        h->platform.channel_observation(h->platform.context, CORE_CHANNEL_PUT,
                                        h->admission.nodes[slot].instance, channel_id(channel),
                                        data, size);
    unlock(h);
    if (result == CORE_DATA_OK)
        wake(h);
    return result;
}

static int32_t channel_get(void *context, size_t slot, const uint64_t channel[3], uint8_t *data,
                           size_t capacity)
{
    core_host *h = context;
    size_t size = 0;
    lock(h);
    const int32_t result = core_channel_get(&h->endpoints, h->admission.nodes[slot].instance,
                                            channel_id(channel), data, capacity, &size);
    if (result == CORE_DATA_OK && h->platform.channel_observation != NULL)
        h->platform.channel_observation(h->platform.context, CORE_CHANNEL_GET,
                                        h->admission.nodes[slot].instance, channel_id(channel),
                                        data, size);
    unlock(h);
    return result == CORE_DATA_OK ? (int32_t)size : result;
}

static int32_t channel_event(void *context, size_t slot, uint64_t channel[3], uint32_t *permissions)
{
    core_host *h = context;
    core_channel_event event;
    lock(h);
    const int32_t result =
        core_channel_event_get(&h->endpoints, h->admission.nodes[slot].instance, &event);
    unlock(h);
    if (result == CORE_DATA_OK) {
        channel[0] = event.channel.source;
        channel[1] = event.channel.destination;
        channel[2] = event.channel.service;
        *permissions = event.permissions;
    }
    return result;
}

static bool channel_ready(void *context, size_t slot)
{
    core_host *h = context;
    lock(h);
    const bool ready = core_channel_event_pending(&h->endpoints, h->admission.nodes[slot].instance);
    unlock(h);
    return ready;
}

static void channel_notify(void *context, size_t slot)
{
    (void)context;
    core_wamr_channel_changed(slot);
}

static bool entry_lock(void *context, size_t slot)
{
    core_host *h = context;
    uint64_t time;
    lock(h);
    /* Workers only check permission. The control task owns lifecycle progress;
     * retain this lock until WAMR publishes entry, including on denial. */
    return now(h, &time) && core_node_start_allowed(&h->nodes, slot, time);
}

static void entry_unlock(void *context)
{
    unlock(context);
}

static bool platform_valid(const core_host_platform *p)
{
    return p != NULL && p->now != NULL && p->lock != NULL && p->unlock != NULL && p->wake != NULL &&
           p->wait != NULL && p->recv != NULL && p->send != NULL && p->retry_us != NULL;
}

static bool enrollment_valid(const core_host_config *c)
{
    if (c == NULL || c->peer_count == 0 || c->peer_count > CORE_MAX_PEERS ||
        c->local_device == c->authority || (c->collector_enabled && c->collector == c->authority))
        return false;
    for (size_t i = 0; i < c->peer_count; ++i)
        if (c->peers[i] == c->authority || (c->collector_enabled && c->peers[i] == c->collector))
            return false;
    return true; /* admission additionally checks uniqueness and local membership */
}

bool core_host_init(core_host *h, const core_host_config *config,
                    const core_host_platform *platform)
{
    if (h == NULL || h->initialized || h->admission.configured || !platform_valid(platform) ||
        !enrollment_valid(config))
        return false;
    if (!core_admission_init(&h->admission, config->node_bytes, config->byte_limit) ||
        !core_admission_configure(&h->admission, config->local_device, config->authority,
                                  config->peers, config->peer_count))
        return false;
    h->platform = *platform;
    h->artifact_interval_us = config->artifact_interval_us;
    h->control = (core_control){.admission = &h->admission,
                                .edges = &h->edges,
                                .collector = config->collector,
                                .collector_enabled = config->collector_enabled};
    h->nodes = (core_node_service){.admission = &h->admission};

    core_endpoint_init(&h->endpoints, &h->admission, channel_notify, h);
    h->edges = (core_edge_service){.admission = &h->admission, .endpoints = &h->endpoints};
    h->ids = (core_coap_ids){.next_mid = 1, .next_token = 1};
    h->active_staging = -1;
    atomic_init(&h->stopping, false);

    for (size_t i = 0; i < config->peer_count; ++i)
        if (config->peers[i] != config->local_device)
            h->peers[h->peer_count++].id = config->peers[i];

    if (!core_wamr_init(&h->nodes.runtime, wake, h))
        return false;
    const core_wamr_channels channels = {.context = h,
                                         .put = channel_put,
                                         .get = channel_get,
                                         .event_get = channel_event,
                                         .event_ready = channel_ready};
    if (!core_wamr_attach_channels(&channels) ||
        !core_wamr_attach_entry_guard(h, entry_lock, entry_unlock)) {
        core_wamr_shutdown();
        return false;
    }
    h->initialized = true;
    return true;
}

/* Only core_host_step schedules paired lifecycle passes. Close expired nodes
 * before constructing edges: channel deadlines may outlast node startup.
 * Then two bounded rounds propagate
 * endpoint READY -> node COMMIT -> endpoint ACTIVE -> node DISPATCH, including
 * entirely local graphs with no incoming packet to trigger another turn.
 * Edges run first so admitted edge removals close dependencies before node
 * startup evidence is considered. No I/O or registry unlock occurs here. */
static void advance_graph_lifecycles_locked(core_host *h, uint64_t time)
{
    core_node_apply_cleanup(&h->nodes, time);
    for (unsigned round = 0; round < 2; ++round) {
        core_edge_step(&h->edges, time);
        core_node_step(&h->nodes, time);
    }
}

static void advance_due_lifecycles_locked(core_host *h, uint64_t time)
{
    if (time >= core_node_next_deadline(&h->nodes) || time >= core_edge_next_deadline(&h->edges))
        advance_graph_lifecycles_locked(h, time);
}

/* Registry lock held on entry and return, temporarily released around free.
 * Only the control task releases staging; its charge prevents node reuse
 * until the buffer is detached, freed and uncharged in that order. */
static bool release_closing_staging(core_host *h)
{
    bool released = false;
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        core_host_staging *s = &h->staging[i];
        if (!s->used || !s->owner->closing)
            continue;

        if (h->active_staging == (int)i) {
            core_coap_exchange_cancel(&h->artifact.exchange);
            h->artifact.transfer = NULL;
            h->active_staging = -1;
        }

        core_artifact_cancel(&s->transfer);
        if (!core_lifetime_unclaimed(s->owner))
            continue;

        uint8_t *bytes = s->bytes;
        s->bytes = NULL;
        unlock(h);
        free(bytes);
        lock(h);

        if (s->charged && !core_lifetime_uncharge(s->owner, s->size))
            continue;
        s->charged = s->used = false;
        released = true;
    }
    return released;
}

static bool needs_artifact(const core_host *h, const core_admission_record *r, uint64_t time)
{
    return r->occupied && !r->endpoint_only && r->request_present && r->node != NULL &&
           !r->cleanup_selected && core_before_deadline(time, r->deadline_us) &&
           !h->nodes.executions[r->node - h->admission.nodes].occupied &&
           !(r->evidence.history & CORE_HISTORY_SUCCESS);
}

static core_host_staging *unused_staging(core_host *h, core_op_id id)
{
    core_host_staging *available = NULL;
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        core_host_staging *s = &h->staging[i];
        if (s->used && s->id.value == id.value)
            return NULL;
        if (!s->used && available == NULL)
            available = s;
    }
    return available;
}

/* Registry lock held on entry and return, temporarily released around malloc.
 * Keep the staging charge and re-sample time before starting the transfer. */
static bool start_next_artifact_download(core_host *h, uint64_t time)
{
    if (h->active_staging >= 0)
        return true;

    for (size_t r = 0; r < CORE_MAX_ADMISSION_RECORDS; ++r) {
        core_admission_record *record = &h->admission.records[r];
        if (!needs_artifact(h, record, time))
            continue;

        core_host_staging *s = unused_staging(h, record->id);
        if (s == NULL)
            continue;

        s->used = true;
        s->id = record->id;
        s->owner = &record->node->lifetime;
        s->size = record->request.artifact_bytes;
        s->charged = core_lifetime_charge(s->owner, s->size);
        if (!s->charged) {
            core_node_artifact_failed(&h->nodes, s->id, time);
            wake(h);
            return true;
        }

        unlock(h);
        s->bytes = malloc(s->size);
        lock(h);
        if (!now(h, &time))
            return false;

        uint64_t token;
        if (s->bytes == NULL || !core_coap_allocate_token(&h->ids, &token) ||
            !core_artifact_begin(&s->transfer, s->owner, s->bytes, s->size, s->size,
                                 record->request.artifact_crc32, token, time,
                                 record->deadline_us)) {
            core_node_artifact_failed(&h->nodes, s->id, time);
            wake(h);
            return true;
        }

        h->active_staging = (int)(s - h->staging);
        h->artifact.transfer = &s->transfer;
        h->artifact.restarted = false;
        return true;
    }
    return true;
}

static void artifact_failure(core_host *h, uint64_t time)
{
    if (h->active_staging < 0)
        return;
    if (!core_artifact_client_restart(&h->artifact, &h->ids, time))
        core_node_artifact_failed(&h->nodes, h->staging[h->active_staging].id, time);
    wake(h);
}

static bool artifact_timeout_due(const core_artifact *transfer, const core_coap_exchange *exchange,
                                 uint64_t time)
{
    if (time >= transfer->deadline_us)
        return true;
    if (!exchange->active)
        return false;
    if (time >= exchange->deadline_us)
        return true;
    return !exchange->acknowledged && exchange->retransmissions == CORE_COAP_MAX_RETRANSMIT &&
           time >= exchange->next_retry_us;
}

static bool send_artifact(core_host *h)
{
    uint64_t time;
    lock(h);
    if (h->active_staging < 0) {
        unlock(h);
        return true;
    }

    core_host_staging *s = &h->staging[h->active_staging];
    const core_admission_record *r = core_admission_find(&h->admission, s->id);
    if (!now(h, &time)) {
        unlock(h);
        return false;
    }

    const core_coap_exchange *exchange = &h->artifact.exchange;
    /* Timeout decisions are never paced, including the terminal retry.
     * Pacing changes send opportunities, not existing deadlines. */
    const bool timeout_due = artifact_timeout_due(&s->transfer, exchange, time);
    if (time < h->next_artifact_send_us && !timeout_due) {
        unlock(h);
        return true;
    }

    const core_coap_poll_result result = core_artifact_client_next(
        &h->artifact, h->admission.authority, r->request.artifact_ref, r->request.artifact_ref_size,
        &h->ids, time, h->platform.retry_us(h->platform.context));
    if (result == CORE_COAP_SEND) {
        h->next_artifact_send_us = h->artifact_interval_us > UINT64_MAX - time
                                       ? UINT64_MAX
                                       : time + h->artifact_interval_us;
        count(&h->stats.artifact_requests);

        /* Only the control task changes exchanges; these bytes remain
         * owned by the exchange throughout the unlocked send. */
        unlock(h);
        const bool sent =
            send_packet(h, CORE_HOST_MANAGEMENT, h->admission.authority,
                        h->artifact.exchange.request, h->artifact.exchange.request_size);
        lock(h);
        if (!now(h, &time)) {
            unlock(h);
            return false;
        }
        if (!sent)
            artifact_failure(h, time);
    } else if (result == CORE_COAP_TIMED_OUT)
        artifact_failure(h, time);

    unlock(h);
    return true;
}

static bool send_management_peers(core_host *h)
{
    uint64_t time;
    for (size_t i = 0; i < h->peer_count; ++i) {
        uint8_t bytes[CORE_COAP_MAX_DATAGRAM];
        size_t size;

        lock(h);
        if (!now(h, &time)) {
            unlock(h);
            return false;
        }

        const bool send = core_control_peer_next(&h->control, &h->peers[i], &h->ids, time,
                                                 h->platform.retry_us(h->platform.context), bytes,
                                                 sizeof(bytes), &size);
        unlock(h);

        if (send)
            (void)send_packet(h, CORE_HOST_MANAGEMENT, h->peers[i].id, bytes, size);
    }
    return true;
}

static bool send_application_batch(core_host *h, bool *exhausted)
{
    for (size_t i = 0; i < CORE_HOST_APPLICATION_TX_BUDGET; ++i) {
        core_data_frame frame;
        uint64_t destination, time;

        lock(h);
        if (!now(h, &time)) {
            unlock(h);
            return false;
        }

        advance_due_lifecycles_locked(h, time);
        const bool available = core_endpoint_next(&h->endpoints, &destination, &frame);
        if (available && destination == h->admission.local_device)
            if (core_endpoint_receive(&h->endpoints, h->admission.local_device, &frame) !=
                CORE_DATA_OK)
                count(&h->stats.endpoint_drops);
        unlock(h);

        if (!available)
            break;

        if (destination != h->admission.local_device) {
            uint8_t bytes[CORE_DATA_FRAME_BYTES];
            size_t size;
            if (core_data_encode(&frame, bytes, sizeof(bytes), &size))
                (void)send_packet(h, CORE_HOST_APPLICATION, destination, bytes, size);
        }
        if (i + 1 == CORE_HOST_APPLICATION_TX_BUDGET)
            *exhausted = true;
    }
    return true;
}

/* Registry lock held; diagnostics must be nonblocking. */
static void runtime_diagnostics_locked(core_host *h)
{
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        if (!h->nodes.executions[i].occupied)
            continue;

        core_runtime_state *state = h->nodes.runtime.lock(h->nodes.runtime.context, i);
        const size_t count = state->errors_pending;
        state->errors_pending = 0;
        h->nodes.runtime.unlock(h->nodes.runtime.context, i);
        if (count)
            diagnostic(h, "ERROR", h->nodes.executions[i].id, count);
    }
}

/* Registry lock held. Detach the finished client before runtime submission.
 * Only successful submission transfers the staging allocation and its charge. */
static void handoff_staged_artifact_locked(core_host *h, core_host_staging *s, uint8_t *bytes,
                                           size_t size, uint64_t time)
{
    h->active_staging = -1;
    h->artifact.transfer = NULL;
    diagnostic(h, "ARTIFACT", s->id, size);

    if (core_node_artifact(&h->nodes, s->id, bytes, size, time)) {
        s->bytes = NULL;
        s->charged = s->used = false;
    } else
        core_node_artifact_failed(&h->nodes, s->id, time);
}

/* Registry lock held; transfer access is released before runtime handoff. */
static bool receive_artifact_locked(core_host *h, uint64_t sender, const core_coap_message *message,
                                    uint64_t time, bool *acknowledge)
{
    const core_artifact_result result =
        core_artifact_client_receive(&h->artifact, sender, message, time, acknowledge);

    uint64_t finished;
    if (!now(h, &finished))
        return false;
    if (finished >= time) {
        maximum(&h->stats.artifact_receive_max_us, finished - time);
        if (result == CORE_ARTIFACT_COMPLETE)
            maximum(&h->stats.artifact_complete_max_us, finished - time);
    }

    if (result == CORE_ARTIFACT_INVALID)
        artifact_failure(h, time);
    if (result != CORE_ARTIFACT_COMPLETE || h->active_staging < 0)
        return true;

    core_host_staging *s = &h->staging[h->active_staging];
    uint8_t *bytes;
    size_t size;
    if (!now(h, &time))
        return false;
    if (!core_artifact_take(&s->transfer, time, &bytes, &size)) {
        artifact_failure(h, time);
        return true;
    }

    handoff_staged_artifact_locked(h, s, bytes, size, time);
    return true;
}

static bool is_response(const core_coap_message *message)
{
    return message->type == CORE_COAP_ACK || message->type == CORE_COAP_RST ||
           (message->code >> 5) >= 2;
}

/* Registry lock held. Incorporate one packet and encode its reply, without
 * performing I/O or scheduling node/edge passes. Control handlers may finish
 * synchronous endpoint work needed to construct a cleanup reply. */
static bool apply_received_packet_locked(core_host *h, const core_host_packet *packet,
                                         uint64_t time, core_host_packet *response)
{
    if (packet->size == 0 || packet->size > sizeof(packet->bytes)) {
        count(&h->stats.invalid_packets[packet->traffic_class]);
        return true;
    }

    response->sender = packet->sender;
    if (packet->traffic_class == CORE_HOST_APPLICATION) {
        core_data_frame frame;
        if (core_data_decode(packet->bytes, packet->size, &frame)) {
            if (core_endpoint_receive(&h->endpoints, packet->sender, &frame) != CORE_DATA_OK)
                count(&h->stats.endpoint_drops);
        } else
            count(&h->stats.invalid_packets[CORE_HOST_APPLICATION]);
        return true;
    }

    core_coap_message message;
    if (packet->bytes[0] == CORE_SCHEMA_VERSION ||
        !core_coap_decode(packet->bytes, packet->size, &message)) {
        count(&h->stats.invalid_packets[CORE_HOST_MANAGEMENT]);
        return true;
    }
    if (!is_response(&message)) {
        (void)core_control_receive_message(&h->control, packet->sender, &message, time,
                                           response->bytes, sizeof(response->bytes),
                                           &response->size);
        return true;
    }

    bool acknowledge = false;
    if (packet->sender == h->admission.authority) {
        if (!receive_artifact_locked(h, packet->sender, &message, time, &acknowledge))
            return false;
    } else
        for (size_t i = 0; i < h->peer_count; ++i) {
            if (h->peers[i].id != packet->sender)
                continue;
            core_control_peer_receive(&h->control, &h->peers[i], &message, time, &acknowledge);
            break;
        }

    if (acknowledge) {
        const core_coap_message ack = {.type = CORE_COAP_ACK, .message_id = message.message_id};
        (void)core_coap_encode(&ack, response->bytes, sizeof(response->bytes), &response->size);
    }
    return true;
}

static uint64_t artifact_deadline(const core_host *h)
{
    if (h->active_staging < 0)
        return UINT64_MAX;
    const core_artifact *transfer = h->artifact.transfer;
    const core_coap_exchange *exchange = &h->artifact.exchange;
    uint64_t next = h->next_artifact_send_us;
    if (exchange->active) {
        next = core_coap_exchange_deadline(exchange);
        /* Pace retry opportunities, but never postpone a terminal timeout. */
        if (!exchange->acknowledged && exchange->retransmissions < CORE_COAP_MAX_RETRANSMIT &&
            next < h->next_artifact_send_us)
            next = h->next_artifact_send_us;
        if (next > exchange->deadline_us)
            next = exchange->deadline_us;
    }
    return next < transfer->deadline_us ? next : transfer->deadline_us;
}

static uint64_t next_deadline(const core_host *h)
{
    uint64_t next = core_node_next_deadline(&h->nodes);
    const uint64_t edge = core_edge_next_deadline(&h->edges);
    if (edge < next)
        next = edge;
    const uint64_t artifact = artifact_deadline(h);
    if (artifact < next)
        next = artifact;
    for (size_t i = 0; i < h->peer_count; ++i) {
        const uint64_t peer = core_coap_exchange_deadline(&h->peers[i].exchange);
        if (peer < next)
            next = peer;
    }
    return next;
}

/* Each consumed datagram spends one budget unit even if the adapter discards
 * it. A full batch requests another round; no new external wake is required. */
static bool receive_batch(core_host *h, core_host_class traffic_class, size_t budget,
                          core_host_packet *responses, size_t *response_count, bool *exhausted)
{
    for (size_t i = 0; i < budget; ++i) {
        core_host_packet packet;
        uint64_t time;
        lock(h);
        if (!now(h, &time)) {
            unlock(h);
            return false;
        }
        advance_due_lifecycles_locked(h, time);
        unlock(h);
        const core_host_event event = h->platform.recv(h->platform.context, traffic_class, &packet);
        switch (event) {
        case CORE_HOST_EMPTY:
            return true;
        case CORE_HOST_STOP:
            atomic_store(&h->stopping, true);
            return true;
        case CORE_HOST_DROPPED:
            count(&h->stats.receive_drops[traffic_class]);
            break;
        case CORE_HOST_PACKET: {
            count(&h->stats.received[traffic_class]);
            if (packet.traffic_class != traffic_class) {
                count(&h->stats.invalid_packets[traffic_class]);
                break;
            }
            /* Application handlers never reply. Management consumes at
             * most one reply slot per budget unit, avoiding a datagram
             * temporary and copy on the embedded control stack. */
            core_host_packet *response = &responses[*response_count];
            response->size = 0;
            lock(h);
            if (!now(h, &time)) {
                unlock(h);
                return false;
            }
            advance_graph_lifecycles_locked(h, time); /* expiry wins after reception too */
            const bool good =
                apply_received_packet_locked(h, &packet, time, response) && now(h, &time);
            if (good)
                advance_graph_lifecycles_locked(h, time);
            unlock(h);
            if (!good)
                return false;
            if (response->size)
                ++*response_count;
            break;
        }
        default:
            return false;
        }
        if (i + 1 == budget)
            *exhausted = true;
    }
    return true;
}

bool core_host_step(core_host *h, uint64_t *deadline)
{
    if (!h->initialized || deadline == NULL)
        return false;

    core_host_packet responses[CORE_HOST_MANAGEMENT_RX_BUDGET];
    size_t response_count = 0;
    bool exhausted = false;
    uint64_t time, started;

    lock(h);
    if (!now(h, &started))
        goto failed;
    time = started;
    advance_graph_lifecycles_locked(h, time); /* expiry wins over queued input */
    unlock(h);

    if (!receive_batch(h, CORE_HOST_APPLICATION, CORE_HOST_APPLICATION_RX_BUDGET, responses,
                       &response_count, &exhausted) ||
        !receive_batch(h, CORE_HOST_MANAGEMENT, CORE_HOST_MANAGEMENT_RX_BUDGET, responses,
                       &response_count, &exhausted))
        return false;

    lock(h);
    if (!now(h, &time))
        goto failed;
    if (h->management_observed && time >= h->last_management_us)
        maximum(&h->stats.max_management_gap_us, time - h->last_management_us);

    h->last_management_us = time;
    h->management_observed = true;

    if (release_closing_staging(h)) {
        if (!now(h, &time))
            goto failed;
        advance_graph_lifecycles_locked(h, time); /* accept released staging before sleeping */
    }

    if (!start_next_artifact_download(h, time))
        goto failed;
    unlock(h);

    /* All output follows the control decisions. Each bounded sender borrows
     * the registry only to select work; network I/O always runs unlocked. */
    if (!send_application_batch(h, &exhausted))
        return false;
    for (size_t i = 0; i < response_count; ++i)
        (void)send_packet(h, CORE_HOST_MANAGEMENT, responses[i].sender, responses[i].bytes,
                          responses[i].size);

    /* The full finite peer pass gives each peer one opportunity per round;
     * there is no deferred partial scan needing a second fairness cursor. */
    if (!send_management_peers(h) || !send_artifact(h))
        return false;

    lock(h);
    runtime_diagnostics_locked(h);
    core_admission_maintain(&h->admission);
    if (!now(h, &time))
        goto failed;
    advance_due_lifecycles_locked(h, time);
    *deadline = exhausted ? 0 : next_deadline(h);
    count(&h->stats.rounds);
    if (time >= started)
        maximum(&h->stats.max_round_us, time - started);
    unlock(h);
    return true;
failed:
    unlock(h);
    return false;
}

bool core_host_run(core_host *h)
{
    while (!atomic_load(&h->stopping)) {
        uint64_t deadline;
        if (!core_host_step(h, &deadline))
            return false;
        if (atomic_load(&h->stopping))
            break;
        if (deadline == 0)
            continue;

        const core_host_event event = h->platform.wait(h->platform.context, deadline);
        if (event == CORE_HOST_STOP)
            break;
        if (event == CORE_HOST_ERROR)
            return false;
    }
    return true;
}

void core_host_request_stop(core_host *h)
{
    atomic_store(&h->stopping, true);
    wake(h);
}

void core_host_destroy(core_host *h)
{
    if (!h->initialized)
        return;

    core_wamr_shutdown(); /* joins every callback before releasing host storage */
    core_coap_exchange_cancel(&h->artifact.exchange);
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        core_host_staging *s = &h->staging[i];
        if (!s->used)
            continue;
        core_lifetime_close(s->owner);
        core_artifact_cancel(&s->transfer);
        free(s->bytes);
        s->bytes = NULL;
    }
    h->initialized = false;
}

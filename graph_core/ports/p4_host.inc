/* P4 additions to the shared host port. One short registry lock serializes
 * runtime copies with admission/closure; socket I/O uses owned message copies. */
#include <pthread.h>

static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static core_endpoint_service endpoints;
static core_edge_service edges = {.admission = &admission, .endpoints = &endpoints};
static struct {
    uint64_t id;
    struct sockaddr_in address;
    core_coap_exchange exchange;
    core_edge_effect effect;
} remote[CORE_MAX_PEERS];
static size_t remote_count;

static void registry_lock(void) { pthread_mutex_lock(&registry_mutex); }
static void registry_unlock(void) { pthread_mutex_unlock(&registry_mutex); }

static core_channel_id logical_channel(const uint64_t channel[3])
{
    return (core_channel_id){channel[0], channel[1], channel[2]};
}

static int32_t channel_put(void *context, size_t slot, const uint64_t channel[3],
                            const uint8_t *data, size_t size)
{
    (void)context;
    registry_lock();
    const int32_t result = core_channel_put(&endpoints, admission.nodes[slot].instance,
                                             logical_channel(channel), data, size);
    registry_unlock();
    if (result == CORE_DATA_OK) runtime_notify(NULL);
    return result;
}

static int32_t channel_get(void *context, size_t slot, const uint64_t channel[3],
                            uint8_t *data, size_t capacity)
{
    (void)context;
    size_t size = 0;
    registry_lock();
    const int32_t result = core_channel_get(&endpoints, admission.nodes[slot].instance,
                                             logical_channel(channel), data, capacity, &size);
    registry_unlock();
    return result == CORE_DATA_OK ? (int32_t)size : result;
}

static int32_t channel_event(void *context, size_t slot, uint64_t channel[3], uint32_t *permissions)
{
    (void)context;
    core_channel_event event;
    registry_lock();
    const int32_t result = core_channel_event_get(&endpoints, admission.nodes[slot].instance, &event);
    registry_unlock();
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
    (void)context;
    registry_lock();
    const bool result = core_channel_event_pending(&endpoints, admission.nodes[slot].instance);
    registry_unlock();
    return result;
}

static void channel_notify(void *context, size_t slot)
{
    (void)context;
    core_wamr_channel_changed(slot);
}

static bool entry_lock(void *context, size_t slot)
{
    (void)context;
    registry_lock();
    uint64_t now;
    if (!core_clock_now(&now)) return false;
    core_edge_step(&edges, now);
    core_node_step(&nodes, now);
    return core_node_start_allowed(&nodes, slot, now);
}

static void entry_unlock(void *context)
{
    (void)context;
    registry_unlock();
}

static bool endpoints_open(void)
{
    core_endpoint_init(&endpoints, &admission, channel_notify, NULL);
    control.edges = &edges;
    const core_wamr_channels channels = {.put = channel_put, .get = channel_get,
        .event_get = channel_event, .event_ready = channel_ready};
    return core_wamr_attach_channels(&channels) &&
        core_wamr_attach_entry_guard(NULL, entry_lock, entry_unlock);
}

static bool edge_send(size_t peer, uint64_t now)
{
    core_edge_effect *effect = &remote[peer].effect;
    uint64_t token;
    if (next_mid > UINT16_MAX || !allocate_token(&token)) return false;
    const char *path = core_edge_path(effect->kind);
    const uint8_t format = 60;
    core_coap_message message = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = (uint16_t)next_mid++, .token_size = 8,
        .options = {{.number = CORE_COAP_URI_PATH, .value = (const uint8_t *)path, .size = strlen(path)},
                    {.number = CORE_COAP_CONTENT_FORMAT, .value = &format, .size = 1}},
        .option_count = 2, .payload = effect->body, .payload_size = effect->size};
    token_bytes(token, message.token);
    uint8_t bytes[CORE_COAP_MAX_DATAGRAM];
    size_t size;
    if (!core_coap_encode(&message, bytes, sizeof(bytes), &size) ||
        !core_coap_exchange_start(&remote[peer].exchange, remote[peer].id, bytes, size,
            now, effect->deadline_us, CORE_COAP_ACK_TIMEOUT_US + (uint64_t)(rand() % 1000001)))
        return false;
    /* A failed nonblocking send is ordinary loss. CoAP retains the command. */
    (void)send_to(bytes, size, &remote[peer].address);
    return true;
}

static void endpoints_service(uint64_t now)
{
    registry_lock();
    core_edge_step(&edges, now);
    registry_unlock();
    for (size_t i = 0; i < remote_count; ++i) {
        core_coap_exchange *x = &remote[i].exchange;
        registry_lock();
        const bool current = !x->active || core_edge_effect_current(&edges, &remote[i].effect, now);
        registry_unlock();
        if (!current) core_coap_exchange_cancel(x);
        if (x->active && core_coap_exchange_poll(x, now) == CORE_COAP_SEND)
            (void)send_to(x->request, x->request_size, &remote[i].address);
        if (!x->active) {
            registry_lock();
            const bool pending = core_edge_next(&edges, remote[i].id, now, &remote[i].effect);
            registry_unlock();
            if (pending && !edge_send(i, now)) {
                registry_lock();
                core_edge_rejected(&edges, &remote[i].effect, now);
                registry_unlock();
            }
        }
    }
    /* Fair bounded batch. A full batch retains a wakeup to drain remaining
     * queues; a busy producer cannot starve control/timers between batches. */
    for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i) {
        core_data_frame frame;
        uint64_t destination;
        registry_lock();
        const bool available = core_endpoint_next(&endpoints, &destination, &frame);
        if (available && destination == admission.local_device)
            (void)core_endpoint_receive(&endpoints, admission.local_device, &frame);
        registry_unlock();
        if (!available) return;
        if (destination != admission.local_device) {
            uint8_t bytes[CORE_DATA_FRAME_BYTES];
            size_t size;
            if (core_data_encode(&frame, bytes, sizeof(bytes), &size))
                for (size_t p = 0; p < remote_count; ++p)
                    if (remote[p].id == destination) (void)send_to(bytes, size, &remote[p].address);
        }
    }
    runtime_notify(NULL);
}

static void endpoint_response(uint64_t sender, const core_coap_message *message, uint64_t now)
{
    for (size_t i = 0; i < remote_count; ++i) {
        if (remote[i].id != sender) continue;
        const core_coap_exchange *x = &remote[i].exchange;
        core_owner_report retained;
        registry_lock();
        const core_admission_record *record = core_admission_find(&admission, remote[i].effect.operation);
        const bool same_incarnation = record != NULL &&
            record->incarnation == remote[i].effect.incarnation &&
            record - admission.records == remote[i].effect.record_slot;
        registry_unlock();
        /* A cleanup reply can carry closure even at/after its transport
         * cutoff. Accept that attributed evidence through the normal guards;
         * it never reopens the exchange or authorizes another retry wave. */
        if (same_incarnation && (message->code >> 5) == 2 && message->token_size == x->token_size &&
            memcmp(message->token, x->token, x->token_size) == 0 &&
            core_owner_report_decode(message->payload, message->payload_size, &retained) &&
            retained.operation.value == remote[i].effect.operation.value &&
            retained.object == remote[i].effect.object) {
            registry_lock();
            (void)core_edge_owner(&edges, sender, message->payload, message->payload_size, now);
            registry_unlock();
        }
        bool acknowledge;
        const core_coap_receive_result result = core_coap_exchange_receive(&remote[i].exchange,
            sender, message, now, &acknowledge);
        if (acknowledge) {
            const core_coap_message ack = {.type = CORE_COAP_ACK, .message_id = message->message_id};
            uint8_t bytes[4];
            size_t size;
            if (core_coap_encode(&ack, bytes, sizeof(bytes), &size))
                (void)send_to(bytes, size, &remote[i].address);
        }
        if (result == CORE_COAP_RESET || (result == CORE_COAP_RESPONSE && message->code >= 128)) {
            registry_lock();
            core_edge_rejected(&edges, &remote[i].effect, now);
            registry_unlock();
        }
        if (result == CORE_COAP_RESPONSE && (message->code >> 5) == 2) {
            registry_lock();
            core_edge_delivered(&edges, &remote[i].effect);
            registry_unlock();
        }
        return;
    }
}

static uint64_t endpoint_deadline(void)
{
    const uint64_t edge_deadline = core_edge_next_deadline(&edges);
    uint64_t next = edge_deadline;
    for (size_t i = 0; i < remote_count; ++i) {
        const core_coap_exchange *x = &remote[i].exchange;
        if (!x->active) continue;
        if (x->deadline_us < next) next = x->deadline_us;
        if (!x->acknowledged && x->next_retry_us < next) next = x->next_retry_us;
    }
    return next;
}

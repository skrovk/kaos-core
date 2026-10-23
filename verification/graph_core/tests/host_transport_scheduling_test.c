#include "admission_fixture.h"
#include "kaos_host.h"
#include <pthread.h>

/* Fake clocks and datagram queues exercise the production service round. No
 * guest is started: established nodes are fixture state and artifact replies
 * stop before the last block. The adapter checks every I/O runs unlocked. */
enum { PACKETS = 16, EVENTS = 4096 };
typedef struct {
    core_host host;
    pthread_mutex_t mutex;
    uint64_t time;
    core_host_packet packets[CORE_HOST_CLASSES][PACKETS], last_get;
    size_t head[CORE_HOST_CLASSES], tail[CORE_HOST_CLASSES];
    core_host_event stream[CORE_HOST_CLASSES];
    char events[EVENTS];
    size_t event_count, app_attempts, gets, replies, wakes, waits;
    uint64_t services[EVENTS];
    uint16_t sizes[EVENTS];
    bool fail_application, fail_artifact, refill;
    core_reserved_node *must_be_closing;
} device;

static void unlocked(device *d)
{
    CHECK(pthread_mutex_trylock(&d->mutex) == 0);
    CHECK(pthread_mutex_unlock(&d->mutex) == 0);
}
static void event(device *d, char value)
{
    CHECK(d->event_count < EVENTS);
    d->events[d->event_count++] = value;
}
static bool clock_now(void *context, uint64_t *time)
{
    *time = ((device *)context)->time;
    return true;
}
static void lock(void *context) { CHECK(pthread_mutex_lock(&((device *)context)->mutex) == 0); }
static void unlock(void *context) { CHECK(pthread_mutex_unlock(&((device *)context)->mutex) == 0); }
static void wake(void *context) { ++((device *)context)->wakes; }
static uint64_t retry(void *context) { (void)context; return CORE_COAP_ACK_TIMEOUT_US; }
static core_host_event wait_input(void *context, uint64_t deadline)
{
    device *d = context;
    (void)deadline;
    unlocked(d);
    ++d->waits;
    return CORE_HOST_STOP;
}
static core_host_event receive_packet(void *context, core_host_class traffic_class,
                                       core_host_packet *packet)
{
    device *d = context;
    unlocked(d);
    CHECK(traffic_class < CORE_HOST_CLASSES);
    if (d->must_be_closing != NULL) CHECK(d->must_be_closing->lifetime.closing);
    core_host_event result;
    if (d->head[traffic_class] < d->tail[traffic_class]) {
        *packet = d->packets[traffic_class][d->head[traffic_class]++];
        result = CORE_HOST_PACKET;
    } else {
        result = d->stream[traffic_class];
        if (result == CORE_HOST_PACKET)
            *packet = (core_host_packet){.sender = 2, .traffic_class = traffic_class,
                                         .size = 1, .bytes = {0}};
    }
    if (result != CORE_HOST_EMPTY) event(d, traffic_class == CORE_HOST_APPLICATION ? 'a' : 'm');
    return result;
}
static bool send_packet(void *context, core_host_class traffic_class, uint64_t peer,
                        const uint8_t *bytes, size_t size)
{
    device *d = context;
    unlocked(d);
    if (traffic_class == CORE_HOST_APPLICATION) {
        core_data_frame frame;
        CHECK(peer == 2 && core_data_decode(bytes, size, &frame));
        CHECK(d->app_attempts < EVENTS);
        d->services[d->app_attempts] = frame.channel.service;
        d->sizes[d->app_attempts++] = frame.size;
        event(d, 'A');
        if (d->refill) {
            lock(d);
            CHECK(core_channel_put(&d->host.endpoints, frame.source_instance, frame.channel,
                frame.payload, frame.size) == CORE_DATA_OK);
            unlock(d);
        }
        return !d->fail_application;
    }
    CHECK(traffic_class == CORE_HOST_MANAGEMENT);
    core_coap_message message;
    CHECK(core_coap_decode(bytes, size, &message));
    if (message.code == CORE_COAP_GET && peer == 99) {
        ++d->gets;
        d->last_get.size = size;
        memcpy(d->last_get.bytes, bytes, size);
        event(d, 'F');
        return !d->fail_artifact;
    }
    ++d->replies;
    event(d, 'M');
    return true;
}
static device *start(uint64_t artifact_interval)
{
    device *d = calloc(1, sizeof(*d));
    CHECK(d != NULL);
    d->time = 10;
    d->stream[CORE_HOST_MANAGEMENT] = d->stream[CORE_HOST_APPLICATION] = CORE_HOST_EMPTY;
    CHECK(pthread_mutex_init(&d->mutex, NULL) == 0);
    const core_host_platform platform = {.context = d, .now = clock_now, .lock = lock,
        .unlock = unlock, .wake = wake, .wait = wait_input, .recv = receive_packet,
        .send = send_packet, .retry_us = retry};
    const core_host_config config = {.local_device = 1, .authority = 99, .peers = {1, 2},
        .peer_count = 2, .node_bytes = 4096,
        .byte_limit = CORE_MAX_NODE_RESERVATIONS * 4096u + CORE_MAX_ENDPOINT_RESERVATIONS * CORE_ENDPOINT_BYTES,
        .artifact_interval_us = artifact_interval};
    CHECK(core_host_init(&d->host, &config, &platform));
    return d;
}
static void finish(device *d)
{
    core_host_destroy(&d->host);
    CHECK(pthread_mutex_destroy(&d->mutex) == 0);
    free(d);
}
static uint64_t step(device *d)
{
    uint64_t deadline = UINT64_MAX;
    CHECK(core_host_step(&d->host, &deadline));
    return deadline;
}
static void queue(device *d, core_host_packet packet)
{
    const core_host_class c = packet.traffic_class;
    CHECK(c < CORE_HOST_CLASSES && d->tail[c] < PACKETS);
    d->packets[c][d->tail[c]++] = packet;
}
static core_admission_record *submit(device *d, const fixture_request *request)
{
    uint8_t bytes[CORE_MAX_REQUEST_BYTES];
    const core_admission_record *record;
    const size_t size = fixture_encode(request, bytes);
    const core_admit_result result = core_request_submit(&d->host.admission, 99, bytes, size, d->time, &record);
    if (result != CORE_ADMIT_ACCEPTED)
        fprintf(stderr, "admission id=%llu kind=%u result=%u\n",
            (unsigned long long)request->id.value, request->scope.kind, result);
    CHECK(result == CORE_ADMIT_ACCEPTED);
    return &d->host.admission.records[record - d->host.admission.records];
}
static core_host_packet management(const fixture_request *request)
{
    uint8_t body[CORE_MAX_REQUEST_BYTES];
    const uint8_t format = 60;
    const size_t size = request == NULL ? 0 : fixture_encode(request, body);
    const core_coap_message message = {.type = CORE_COAP_CON,
        .code = request == NULL ? CORE_COAP_GET : CORE_COAP_POST, .message_id = 42,
        .token_size = 1, .token = {7}, .option_count = request == NULL ? 1 : 2,
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)(request == NULL ? "none" : "graph"),
                     request == NULL ? 4 : 5}, {CORE_COAP_CONTENT_FORMAT, &format, 1}},
        .payload = request == NULL ? NULL : body, .payload_size = size};
    core_host_packet packet = {.sender = 99, .traffic_class = CORE_HOST_MANAGEMENT};
    CHECK(core_coap_encode(&message, packet.bytes, sizeof(packet.bytes), &packet.size));
    return packet;
}
static core_admission_record *established_node(device *d)
{
    const fixture_request request = fixture_node(1, 10);
    core_admission_record *r = submit(d, &request);
    CHECK(core_report_observe(&r->evidence, 0, CORE_OBJECT_READY) == CORE_REPORT_CHANGED);
    CHECK(core_report_record(&r->evidence, CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH |
        CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS) == CORE_REPORT_CHANGED);
    CHECK(core_admission_establish_node(&d->host.admission, (core_instance_id){1}));
    return r;
}
static core_binding sender(device *d, uint64_t id, uint64_t service)
{
    fixture_request request = {.id = {id}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    request.scope.bindings[0].channel = fixture_channel(10, 20, 1, 2, 1, 2);
    request.scope.bindings[0].channel.service = service;
    core_admission_record *r = submit(d, &request);
    core_endpoint_step(&d->host.endpoints);
    CHECK(core_endpoint_activate(&d->host.endpoints, &r->scope.bindings[0], true));
    /* Recorded remote completion is fixture state; no second host is needed
     * to establish endpoint scheduling and best-effort dequeue semantics. */
    for (unsigned i = 0; i < 2; ++i)
        CHECK(core_report_observe(&r->evidence, i, CORE_OBJECT_READY | CORE_OBJECT_ACTIVE) == CORE_REPORT_CHANGED);
    CHECK(core_report_record(&r->evidence, CORE_HISTORY_COMMIT | CORE_HISTORY_SUCCESS) == CORE_REPORT_CHANGED);
    return r->scope.bindings[0];
}
static void put(device *d, uint64_t service, size_t size)
{
    const uint8_t payload[16] = {1, 2, 3};
    CHECK(core_channel_put(&d->host.endpoints, (core_instance_id){1},
        (core_channel_id){10, 20, service}, payload, size) == CORE_DATA_OK);
}
static core_admission_record *artifact(device *d, uint64_t allowance)
{
    fixture_request request = fixture_node(100, 30);
    const uint8_t bytes[512] = {0};
    request.node.artifact_bytes = sizeof(bytes);
    request.node.required_bytes = 4096;
    request.node.artifact_crc32 = core_artifact_crc32(bytes, sizeof(bytes));
    request.node.addition_allowance_us = allowance;
    return submit(d, &request);
}
static core_host_packet first_block(device *d)
{
    core_coap_message request;
    CHECK(core_coap_decode(d->last_get.bytes, d->last_get.size, &request));
    const uint8_t etag = 1, format = 42, block = 12, size[] = {2, 0};
    const uint8_t bytes[256] = {0};
    core_coap_message response = {.type = CORE_COAP_ACK, .code = CORE_COAP_CONTENT,
        .message_id = request.message_id, .token_size = request.token_size, .option_count = 4,
        .options = {{CORE_COAP_ETAG, &etag, 1}, {CORE_COAP_CONTENT_FORMAT, &format, 1},
                    {CORE_COAP_BLOCK2, &block, 1}, {CORE_COAP_SIZE2, size, sizeof(size)}},
        .payload = bytes, .payload_size = sizeof(bytes)};
    memcpy(response.token, request.token, request.token_size);
    core_host_packet packet = {.sender = 99, .traffic_class = CORE_HOST_MANAGEMENT};
    CHECK(core_coap_encode(&response, packet.bytes, sizeof(packet.bytes), &packet.size));
    return packet;
}

static void recurring_receive_budgets(void)
{
    device *d = start(1000);
    d->stream[CORE_HOST_APPLICATION] = CORE_HOST_PACKET; /* malformed packets still cost work */
    d->stream[CORE_HOST_MANAGEMENT] = CORE_HOST_DROPPED; /* unknown or oversized datagrams too */
    for (unsigned round = 0; round < 3; ++round) {
        CHECK(step(d) == 0);
        CHECK(d->event_count == 5u * (round + 1));
        CHECK(memcmp(d->events + 5 * round, "aaaam", 5) == 0);
    }
    CHECK(d->host.stats.received[CORE_HOST_APPLICATION] == 12);
    CHECK(d->host.stats.invalid_packets[CORE_HOST_APPLICATION] == 12);
    CHECK(d->host.stats.receive_drops[CORE_HOST_MANAGEMENT] == 3);
    CHECK(d->host.stats.rounds == 3);
    d->stream[CORE_HOST_APPLICATION] = d->stream[CORE_HOST_MANAGEMENT] = CORE_HOST_EMPTY;
    CHECK(step(d) == UINT64_MAX); /* exhausted batches do not leave a permanent immediate deadline */
    d->host.stats.rounds = UINT64_MAX;
    d->host.stats.received[CORE_HOST_APPLICATION] = UINT64_MAX;
    d->host.stats.invalid_packets[CORE_HOST_APPLICATION] = UINT64_MAX;
    d->stream[CORE_HOST_APPLICATION] = CORE_HOST_PACKET;
    CHECK(step(d) == 0);
    CHECK(d->host.stats.rounds == UINT64_MAX);
    CHECK(d->host.stats.received[CORE_HOST_APPLICATION] == UINT64_MAX);
    CHECK(d->host.stats.invalid_packets[CORE_HOST_APPLICATION] == UINT64_MAX);
    finish(d);
}
static void rejects_cross_class_packets(void)
{
    device *d = start(1000);
    core_host_packet command = management(NULL);
    command.traffic_class = CORE_HOST_APPLICATION;
    queue(d, command);
    const core_data_frame frame = {.channel = {10, 20, 7}, .creating_op = {2},
        .source_instance = {1}, .destination_instance = {2}, .size = 1, .payload = {9}};
    core_host_packet packet = {.sender = 2, .traffic_class = CORE_HOST_MANAGEMENT};
    CHECK(core_data_encode(&frame, packet.bytes, sizeof(packet.bytes), &packet.size));
    queue(d, packet);
    CHECK(step(d) == 0);
    CHECK(d->replies == 0 && d->gets == 0 && d->app_attempts == 0);
    CHECK(d->host.stats.invalid_packets[CORE_HOST_APPLICATION] == 1);
    CHECK(d->host.stats.invalid_packets[CORE_HOST_MANAGEMENT] == 1);
    CHECK(step(d) == UINT64_MAX);
    finish(d);
}
static void remaining_input_does_not_need_an_external_wake(void)
{
    device *d = start(1000);
    for (unsigned i = 0; i < 9; ++i)
        queue(d, (core_host_packet){.sender = 2, .traffic_class = CORE_HOST_APPLICATION,
                                   .size = 1, .bytes = {0}});
    for (unsigned i = 0; i < 2; ++i)
        queue(d, (core_host_packet){.sender = 99, .traffic_class = CORE_HOST_MANAGEMENT,
                                   .size = 1, .bytes = {0}});
    CHECK(core_host_run(&d->host));
    CHECK(d->host.stats.rounds == 3 && d->waits == 1);
    CHECK(d->head[CORE_HOST_APPLICATION] == 9 && d->head[CORE_HOST_MANAGEMENT] == 2);
    CHECK(d->event_count == 11 && memcmp(d->events, "aaaamaaaama", 11) == 0);
    finish(d);
}
static void recurring_output_and_round_robin(void)
{
    device *d = start(1000);
    (void)established_node(d);
    (void)sender(d, 2, 7);
    (void)sender(d, 3, 8);
    (void)artifact(d, 10000000);
    put(d, 7, 1);
    put(d, 8, 16); /* fairness is per message, not byte share */
    d->refill = true;
    for (unsigned round = 0; round < 3; ++round) {
        const size_t before = d->event_count;
        queue(d, management(NULL));
        CHECK(step(d) == 0);
        CHECK(d->events[before] == 'm');
        for (size_t i = 0; i < CORE_MAX_ENDPOINT_RESERVATIONS; ++i)
            CHECK(d->events[before + 1 + i] == 'A');
        CHECK(d->events[before + 1 + CORE_MAX_ENDPOINT_RESERVATIONS] == 'M');
        CHECK(d->app_attempts == (round + 1) * CORE_MAX_ENDPOINT_RESERVATIONS);
        CHECK(d->replies == round + 1);
    }
    CHECK(d->gets == 1 && d->host.stats.artifact_requests == 1);
    CHECK(d->events[2 + CORE_MAX_ENDPOINT_RESERVATIONS] == 'F');
    for (size_t i = 0; i < d->app_attempts; ++i) {
        CHECK(d->services[i] == (i % 2 ? 8u : 7u));
        CHECK(d->sizes[i] == (i % 2 ? 16u : 1u));
    }
    d->refill = false;
    d->fail_application = true;
    const size_t attempts = d->app_attempts;
    (void)step(d);
    CHECK(d->app_attempts == attempts + 2);
    CHECK(d->host.stats.send_failures[CORE_HOST_APPLICATION] == 2);
    CHECK(d->host.stats.sent[CORE_HOST_APPLICATION] == attempts);
    (void)step(d);
    CHECK(d->app_attempts == attempts + 2); /* failed best-effort sends were dequeued */
    finish(d);
}
static void endpoint_cursor_persists_between_rounds(void)
{
    device *d = start(1000);
    (void)established_node(d);
    for (unsigned i = 0; i < 3; ++i) {
        (void)sender(d, 2 + i, 7 + i);
        put(d, 7 + i, 1 + i);
    }
    d->refill = true;
    for (unsigned i = 0; i < 3; ++i) CHECK(step(d) == 0);
    CHECK(d->app_attempts == 3 * CORE_MAX_ENDPOINT_RESERVATIONS);
    /* Both capacity profiles have a batch length indivisible by three. A
     * cursor reset at the round boundary would give the first sender extra
     * service and fail this continuous sequence. */
    CHECK(CORE_MAX_ENDPOINT_RESERVATIONS % 3 != 0);
    for (size_t i = 0; i < d->app_attempts; ++i) CHECK(d->services[i] == 7 + i % 3);
    finish(d);
}
static void management_removal_precedes_output(void)
{
    device *d = start(1000);
    (void)established_node(d);
    const core_binding binding = sender(d, 2, 7);
    put(d, 7, 1);
    const fixture_request removal = {.id = {3}, .scope = {.kind = CORE_REQUEST_REMOVE_EDGE,
        .channel_count = 1, .allowance_us = 1000, .bindings = {binding}}};
    queue(d, management(&removal));
    (void)step(d);
    CHECK(d->app_attempts == 0);
    CHECK(core_channel_put(&d->host.endpoints, (core_instance_id){1},
        (core_channel_id){10, 20, 7}, "x", 1) == CORE_DATA_DISABLED);
    CHECK(core_admission_find(&d->host.admission, (core_op_id){3}) != NULL);
    finish(d);
}
static void expiry_precedes_queued_application(void)
{
    device *d = start(1000);
    const fixture_request request = fixture_node(1, 10);
    core_admission_record *node = submit(d, &request);
    CHECK(core_admission_establish_node(&d->host.admission, (core_instance_id){1}));
    fixture_request edge = {.id = {2}, .scope = {.kind = CORE_REQUEST_ADD_EDGE,
        .channel_count = 1, .cleanup_allowance_us = 50}};
    edge.scope.bindings[0].channel = fixture_channel(20, 10, 2, 1, 2, 1);
    uint8_t bytes[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(&edge, bytes);
    const core_admission_record *r;
    CHECK(core_endpoint_submit(&d->host.admission, 2, bytes, size, d->time, &r) == CORE_ADMIT_ACCEPTED);
    core_endpoint_step(&d->host.endpoints);
    CHECK(core_endpoint_activate(&d->host.endpoints, &r->scope.bindings[0], false));
    const core_data_frame frame = {.channel = {20, 10, 7}, .creating_op = {2},
        .source_instance = {2}, .destination_instance = {1}, .size = 1, .payload = {9}};
    core_host_packet packet = {.sender = 2, .traffic_class = CORE_HOST_APPLICATION};
    CHECK(core_data_encode(&frame, packet.bytes, sizeof(packet.bytes), &packet.size));
    queue(d, packet);
    core_access access;
    CHECK(core_access_acquire(&node->node->lifetime, &access));
    d->must_be_closing = node->node;
    d->time = node->deadline_us;
    (void)step(d);
    CHECK(node->cleanup_selected && (node->evidence.objects[0] & CORE_OBJECT_EXPIRED));
    uint8_t output;
    size_t written = 99;
    CHECK(core_channel_get(&d->host.endpoints, (core_instance_id){1}, frame.channel,
        &output, 1, &written) == CORE_DATA_DISABLED && written == 99);
    CHECK(core_access_release(&d->must_be_closing->lifetime, access));
    d->must_be_closing = NULL;
    finish(d);
}
static void blocks_obey_pacing_without_busy_deadlines(void)
{
    device *d = start(1000);
    const core_admission_record *r = artifact(d, 10000000);
    const uint64_t original_deadline = r->deadline_us;
    (void)step(d);
    CHECK(d->gets == 1);
    const core_host_packet response = first_block(d);
    queue(d, response);
    CHECK(step(d) == 0 && d->gets == 1);
    CHECK(step(d) == 1010);
    CHECK(d->host.artifact.transfer->received_size == 256);
    queue(d, response); /* completed exchange's delayed duplicate cannot advance it */
    (void)step(d);
    CHECK(d->host.artifact.transfer->received_size == 256 && d->gets == 1);
    CHECK(step(d) == 1010);
    d->time = 1009;
    CHECK(step(d) == 1010 && d->gets == 1);
    d->time = 1010;
    (void)step(d);
    CHECK(d->gets == 2);
    core_coap_message message;
    uint32_t block;
    CHECK(core_coap_decode(d->last_get.bytes, d->last_get.size, &message));
    CHECK(core_coap_option_uint(core_coap_find(&message, CORE_COAP_BLOCK2), &block) && block == 20);
    CHECK(r->deadline_us == original_deadline);
    CHECK(d->host.artifact.exchange.deadline_us == original_deadline);
    finish(d);
}
static void retries_and_terminal_timeout_keep_deadlines(void)
{
    const uint64_t interval = 40000000;
    device *d = start(interval);
    const core_admission_record *r = artifact(d, 300000000);
    const uint64_t deadline = r->deadline_us;
    (void)step(d);
    d->time = 10 + CORE_COAP_ACK_TIMEOUT_US;
    CHECK(step(d) == 10 + interval && d->gets == 1);
    for (unsigned attempt = 1; attempt <= CORE_COAP_MAX_RETRANSMIT; ++attempt) {
        d->time = 10 + attempt * interval;
        (void)step(d);
        CHECK(d->gets == 1 + attempt);
        CHECK(d->host.artifact.exchange.retransmissions == attempt);
        CHECK(d->host.artifact.exchange.deadline_us == deadline && r->deadline_us == deadline);
    }
    /* The terminal retry cutoff falls before the next paced send permission. */
    const uint64_t terminal = d->host.artifact.exchange.next_retry_us;
    CHECK(terminal < d->time + interval);
    CHECK(step(d) == terminal);
    d->time = terminal;
    CHECK(step(d) == 10 + (CORE_COAP_MAX_RETRANSMIT + 1u) * interval);
    CHECK(d->gets == CORE_COAP_MAX_RETRANSMIT + 1u);
    CHECK(d->host.artifact.restarted && !d->host.artifact.exchange.active);
    CHECK(r->deadline_us == deadline);
    finish(d);
}
static void failed_attempt_and_expiry_preserve_pacing(void)
{
    device *d = start(1000);
    const core_admission_record *r = artifact(d, 10000000);
    d->fail_artifact = true;
    CHECK(step(d) == 1010);
    CHECK(d->gets == 1 && d->host.artifact.restarted);
    CHECK(d->host.stats.send_failures[CORE_HOST_MANAGEMENT] == 1);
    CHECK(step(d) == 1010 && d->gets == 1);
    d->fail_artifact = false;
    d->time = 1010;
    (void)step(d);
    CHECK(d->gets == 2 && d->host.stats.artifact_requests == 2);
    CHECK(d->host.artifact.exchange.deadline_us == r->deadline_us);
    finish(d);

    d = start(1000);
    r = artifact(d, 100);
    (void)step(d);
    queue(d, first_block(d));
    (void)step(d);
    CHECK(step(d) == r->deadline_us);
    d->time = r->deadline_us;
    (void)step(d);
    CHECK(d->gets == 1 && d->host.active_staging == -1);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(d->host.admission.reserved_bytes == 0);
    finish(d);
}
int main(void)
{
    recurring_receive_budgets();
    rejects_cross_class_packets();
    remaining_input_does_not_need_an_external_wake();
    recurring_output_and_round_robin();
    endpoint_cursor_persists_between_rounds();
    management_removal_precedes_output();
    expiry_precedes_queued_application();
    blocks_obey_pacing_without_busy_deadlines();
    retries_and_terminal_timeout_keep_deadlines();
    failed_attempt_and_expiry_preserve_pacing();
    puts("transport scheduling: bounded rounds, class isolation, lifecycle ordering, best effort and artifact pacing passed");
}

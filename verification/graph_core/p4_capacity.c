#define _POSIX_C_SOURCE 200809L
#include "tests/admission_fixture.h"
#include "kaos_edge.h"
#include "kaos_node.h"
#include "kaos_wamr.h"

#include <pthread.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

_Static_assert(CORE_MAX_NODE_RESERVATIONS == 4 && CORE_MAX_ENDPOINT_RESERVATIONS == 32 &&
               CORE_MAX_PORTS == 8 && CORE_MAX_REQUEST_CHANNELS == 8,
               "Build this fixture with CORE_FOUR_NODE_PROFILE");

#ifndef P4_CAPACITY_ARENA_BYTES
#ifdef ESP_PLATFORM
#define P4_CAPACITY_ARENA_BYTES 26624u
#else
#define P4_CAPACITY_ARENA_BYTES 49152u
#endif
#endif
#define NODES 4u
#define CHANNELS 16u
#define ALLOWANCE_US UINT64_C(10000000)

static struct {
    core_admission admission;
    core_endpoint_service endpoints;
    core_edge_service edges;
    core_node_service nodes;
    core_op_id edge_ids[CHANNELS];
    core_op_id node_ids[NODES];
    uint64_t next_id;
    uint64_t puts[NODES], gets[NODES], full[NODES], validated[NODES], deliveries;
#ifdef ESP_PLATFORM
    size_t worker_stack_free[NODES];
    TaskHandle_t worker_task[NODES];
#endif
    bool hold_reads, hold_transfer;
} fixture;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
#ifdef ESP_PLATFORM
static StaticSemaphore_t wake_storage;
static SemaphoreHandle_t changed;
static void memory_snapshot(const char *phase)
{
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    printf("memory phase=%s heap_total=%zu heap_free=%zu heap_min_free=%zu largest=%zu exec_free=%zu\n",
           phase, heap_caps_get_total_size(caps), heap_caps_get_free_size(caps),
           heap_caps_get_minimum_free_size(caps), heap_caps_get_largest_free_block(caps),
           heap_caps_get_free_size(MALLOC_CAP_EXEC));
}

static void task_stacks(void)
{
    TaskStatus_t tasks[16];
    const UBaseType_t count = uxTaskGetSystemState(tasks, 16, NULL);
    CHECK(count != 0);
    for (UBaseType_t i = 0; i < count; ++i)
        printf("memory task=%s number=%u stack_free=%u\n", tasks[i].pcTaskName,
               (unsigned)tasks[i].xTaskNumber, (unsigned)tasks[i].usStackHighWaterMark);
}
#else
static pthread_mutex_t wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool pending_wake;
#endif

static uint64_t now_us(void)
{
    uint64_t now;
    CHECK(core_clock_now(&now));
    return now;
}

static void wake_control(void *context)
{
    (void)context;
#ifdef ESP_PLATFORM
    (void)xSemaphoreGive(changed);
#else
    pthread_mutex_lock(&wake_mutex);
    pending_wake = true;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&wake_mutex);
#endif
}

static void wait_control(void)
{
#ifdef ESP_PLATFORM
    (void)xSemaphoreTake(changed, pdMS_TO_TICKS(10));
#else
    struct timespec until;
    CHECK(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_nsec += 10000000;
    if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&wake_mutex);
    if (!pending_wake) (void)pthread_cond_timedwait(&changed, &wake_mutex, &until);
    pending_wake = false;
    pthread_mutex_unlock(&wake_mutex);
#endif
}

static void channel_changed(void *context, size_t slot)
{
    (void)context;
    core_wamr_channel_changed(slot);
    wake_control(NULL);
}

static core_channel_id channel_id(const uint64_t channel[3])
{
    return (core_channel_id){channel[0], channel[1], channel[2]};
}

static void sample_worker_stack(size_t slot)
{
#ifdef ESP_PLATFORM
    fixture.worker_task[slot] = xTaskGetCurrentTaskHandle();
    /* FreeRTOS retains the deepest stack use, so sparse reads include earlier
     * WAMR preparation/configuration and keep measurement overhead bounded. */
    if ((fixture.puts[slot] + fixture.gets[slot] + fixture.full[slot]) % 128 == 0) {
        const size_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
        if (free_bytes < fixture.worker_stack_free[slot]) fixture.worker_stack_free[slot] = free_bytes;
    }
#else
    (void)slot;
#endif
}

static int32_t guest_put(void *context, size_t slot, const uint64_t channel[3],
                          const uint8_t *data, size_t size)
{
    (void)context;
    pthread_mutex_lock(&registry);
    const core_data_result result = core_channel_put(&fixture.endpoints,
        fixture.admission.nodes[slot].instance, channel_id(channel), data, size);
    sample_worker_stack(slot);
    if (result == CORE_DATA_OK) ++fixture.puts[slot];
    if (result == CORE_DATA_FULL) ++fixture.full[slot];
    pthread_mutex_unlock(&registry);
    if (result == CORE_DATA_OK) wake_control(NULL);
    return result;
}

static int32_t guest_get(void *context, size_t slot, const uint64_t channel[3],
                          uint8_t *data, size_t capacity)
{
    (void)context;
    pthread_mutex_lock(&registry);
    size_t written = 0;
    const core_data_result result = fixture.hold_reads ? CORE_DATA_EMPTY :
        core_channel_get(&fixture.endpoints, fixture.admission.nodes[slot].instance,
                          channel_id(channel), data, capacity, &written);
    sample_worker_stack(slot);
    if (result == CORE_DATA_OK) ++fixture.gets[slot];
    pthread_mutex_unlock(&registry);
    return result == CORE_DATA_OK ? (int32_t)written : result;
}

static int32_t guest_event(void *context, size_t slot, uint64_t channel[3], uint32_t *permissions)
{
    (void)context;
    pthread_mutex_lock(&registry);
    core_channel_event event;
    const core_data_result result = core_channel_event_get(&fixture.endpoints,
        fixture.admission.nodes[slot].instance, &event);
    if (result == CORE_DATA_OK) {
        channel[0] = event.channel.source;
        channel[1] = event.channel.destination;
        channel[2] = event.channel.service;
        *permissions = event.permissions;
    }
    pthread_mutex_unlock(&registry);
    return result;
}

static bool guest_event_ready(void *context, size_t slot)
{
    (void)context;
    pthread_mutex_lock(&registry);
    const bool ready = core_channel_event_pending(&fixture.endpoints, fixture.admission.nodes[slot].instance);
    pthread_mutex_unlock(&registry);
    return ready;
}

static void drive(void)
{
    pthread_mutex_lock(&registry);
    const uint64_t now = now_us();
    core_node_step(&fixture.nodes, now);
    core_edge_step(&fixture.edges, now);
    core_node_step(&fixture.nodes, now); /* endpoint release can unblock node release */
    for (size_t n = 0; n < NODES; ++n) {
        core_runtime_state *state = fixture.nodes.runtime.lock(fixture.nodes.runtime.context, n);
        fixture.validated[n] += state->errors_pending;
        state->errors_pending = 0;
        fixture.nodes.runtime.unlock(fixture.nodes.runtime.context, n);
    }
    if (!fixture.hold_transfer) {
        for (size_t batch = 0; batch < CHANNELS; ++batch) {
            core_data_frame frame;
            uint64_t owner;
            if (!core_endpoint_next(&fixture.endpoints, &owner, &frame)) break;
            CHECK(owner == 1);
            const core_data_result received = core_endpoint_receive(&fixture.endpoints, 1, &frame);
            CHECK(received == CORE_DATA_OK || received == CORE_DATA_FULL || received == CORE_DATA_DISABLED);
            if (received == CORE_DATA_OK) ++fixture.deliveries;
            if (batch + 1 == CHANNELS) wake_control(NULL);
        }
    }
    pthread_mutex_unlock(&registry);
}

static core_op_id next_id(void)
{
    CHECK(fixture.next_id != UINT64_MAX);
    return (core_op_id){++fixture.next_id};
}

static const core_admission_record *submit(const fixture_request *request)
{
    uint8_t encoded[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(request, encoded);
    const core_admission_record *record;
    CHECK(core_request_submit(&fixture.admission, 99, encoded, size, now_us(), &record) == CORE_ADMIT_ACCEPTED);
    return record;
}

static void await_success(core_op_id id)
{
    const uint64_t deadline = now_us() + ALLOWANCE_US;
    for (;;) {
        drive();
        pthread_mutex_lock(&registry);
        const core_admission_record *record = core_admission_find(&fixture.admission, id);
        const core_outcome outcome = core_report_outcome(&record->evidence);
        if (outcome != CORE_OUTCOME_PENDING && outcome != CORE_OUTCOME_SUCCEEDED)
            printf("capacity failed op=%llu kind=%u outcome=%u facts=%u history=%u\n",
                   (unsigned long long)id.value, record->scope.kind, outcome,
                   record->evidence.objects[0], record->evidence.history);
        pthread_mutex_unlock(&registry);
        CHECK(outcome == CORE_OUTCOME_PENDING || outcome == CORE_OUTCOME_SUCCEEDED);
        if (outcome == CORE_OUTCOME_SUCCEEDED) return;
        CHECK(now_us() < deadline);
        wait_control();
    }
}

static void encode64(uint8_t *out, uint64_t value)
{
    for (unsigned b = 0; b < 8; ++b) out[b] = (uint8_t)(value >> (8 * b));
}

static void add_nodes(const uint8_t *artifact, size_t artifact_size)
{
    for (size_t n = 0; n < NODES; ++n) {
        fixture.node_ids[n] = next_id();
        fixture_request request = fixture_node(fixture.node_ids[n].value, 100 + n);
        request.node.artifact_bytes = (uint32_t)artifact_size;
        request.node.required_bytes = (uint32_t)fixture.admission.node_bytes;
        request.node.addition_allowance_us = request.node.cleanup_allowance_us = ALLOWANCE_US;
        request.node.configuration_size = 24;
        encode64(request.node.configuration, 100 + n);
        encode64(request.node.configuration + 8, 100 + (n + NODES - 1) % NODES);
        encode64(request.node.configuration + 16, 100 + (n + 1) % NODES);
        request.scope.port_count = 8;
        for (size_t p = 0; p < 8; ++p)
            request.scope.ports[p] = (core_port){.id = (uint16_t)(p + 1), .type = 3,
                .direction = p < 4 ? CORE_PORT_OUTPUT : CORE_PORT_INPUT, .max_payload = CORE_MAX_PAYLOAD_BYTES};
#ifdef ESP_PLATFORM
        printf("capacity before_node=%zu free=%zu largest=%zu arena=%u artifact=%zu native_stack=%u\n",
               n, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               P4_CAPACITY_ARENA_BYTES, artifact_size, CORE_WAMR_NATIVE_STACK_BYTES);
#endif
        uint8_t *copy = malloc(artifact_size);
        CHECK(copy != NULL);
        memcpy(copy, artifact, artifact_size);
        pthread_mutex_lock(&registry);
        const core_admission_record *record = submit(&request);
        CHECK(core_lifetime_charge(&record->node->lifetime, artifact_size));
        CHECK(core_node_artifact(&fixture.nodes, record->id, copy, artifact_size, now_us()));
        pthread_mutex_unlock(&registry);
    }
    for (size_t n = 0; n < NODES; ++n) await_success(fixture.node_ids[n]);
}

static void add_edges(void)
{
    for (size_t n = 0; n < NODES; ++n) {
        for (size_t c = 0; c < 4; ++c) {
            const size_t next = (n + 1) % NODES;
            fixture_request request = {.id = next_id(), .scope = {.kind = CORE_REQUEST_ADD_EDGE,
                .channel_count = 1, .cleanup_allowance_us = ALLOWANCE_US}};
            core_channel *channel = &request.scope.bindings[0].channel;
            *channel = fixture_channel(100 + n, 100 + next, fixture.node_ids[n].value,
                                       fixture.node_ids[next].value, 1, 1);
            channel->source_port = (uint16_t)(c + 1);
            channel->destination_port = (uint16_t)(c + 5);
            channel->service = 7 + c;
            channel->payload_bytes = CORE_MAX_PAYLOAD_BYTES;
            channel->queue_messages = CORE_MAX_QUEUE_MESSAGES;
            channel->addition_allowance_us = ALLOWANCE_US;
            pthread_mutex_lock(&registry);
            const core_admission_record *record = submit(&request);
            fixture.edge_ids[4 * n + c] = record->id;
            pthread_mutex_unlock(&registry);
            await_success(request.id);
        }
    }
}

static size_t full_queues(bool senders_only)
{
    size_t full = 0;
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e)
        if (fixture.admission.endpoints[e].occupied &&
            (!senders_only || fixture.admission.endpoints[e].sender) &&
            fixture.endpoints.queues[e].count == CORE_MAX_QUEUE_MESSAGES) ++full;
    return full;
}

static void fill_all_queues(void)
{
    const uint64_t deadline = now_us() + ALLOWANCE_US;
    bool receivers_filled = false;
    for (;;) {
        drive();
        pthread_mutex_lock(&registry);
        const size_t senders = full_queues(true), all = full_queues(false);
        if (fixture.hold_transfer && !receivers_filled && senders == CHANNELS)
            fixture.hold_transfer = false;
        if (!fixture.hold_transfer && all - senders == CHANNELS) {
            receivers_filled = true;
            fixture.hold_transfer = true;
        }
        pthread_mutex_unlock(&registry);
        if (all == CORE_MAX_ENDPOINT_RESERVATIONS) break;
        CHECK(now_us() < deadline);
        wait_control();
    }
    printf("capacity queues_full=%u payload=%u messages_per_queue=%u reserved=%zu registry=%zu endpoints=%zu edges=%zu nodes=%zu arena=%u artifact=%zu native_stack=%u\n",
           CORE_MAX_ENDPOINT_RESERVATIONS, CORE_MAX_PAYLOAD_BYTES, CORE_MAX_QUEUE_MESSAGES,
           fixture.admission.reserved_bytes, sizeof(fixture.admission), sizeof(fixture.endpoints),
           sizeof(fixture.edges), sizeof(fixture.nodes), P4_CAPACITY_ARENA_BYTES,
           fixture.admission.node_bytes - P4_CAPACITY_ARENA_BYTES, CORE_WAMR_NATIVE_STACK_BYTES);
#ifdef ESP_PLATFORM
    memory_snapshot("all_queues_full");
    task_stacks();
    printf("capacity full_free=%zu largest=%zu control_stack_free=%u\n",
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    pthread_mutex_lock(&registry);
    fixture.hold_reads = fixture.hold_transfer = false;
    pthread_mutex_unlock(&registry);
}

static void exchange_traffic(void)
{
    const uint64_t started = now_us(), deadline = started + ALLOWANCE_US;
    for (;;) {
        drive();
        pthread_mutex_lock(&registry);
        bool witnessed = true;
        for (size_t n = 0; n < NODES; ++n)
            witnessed &= fixture.gets[n] != 0 && fixture.validated[n] != 0;
        const bool done = fixture.deliveries >= 1000 && witnessed && now_us() - started >= 1000000;
        pthread_mutex_unlock(&registry);
        if (done) break;
        CHECK(now_us() < deadline);
        wait_control();
    }
    pthread_mutex_lock(&registry);
    for (size_t n = 0; n < NODES; ++n) {
        core_runtime_state *state = fixture.nodes.runtime.lock(fixture.nodes.runtime.context, n);
        CHECK(state->main_active && !state->exited && !state->fault);
        fixture.nodes.runtime.unlock(fixture.nodes.runtime.context, n);
        printf("capacity node=%zu puts=%llu gets=%llu full=%llu guest_validated=%llu\n", n,
               (unsigned long long)fixture.puts[n], (unsigned long long)fixture.gets[n],
               (unsigned long long)fixture.full[n], (unsigned long long)fixture.validated[n]);
#ifdef ESP_PLATFORM
        printf("capacity node=%zu worker_stack_free=%zu\n", n, fixture.worker_stack_free[n]);
        CHECK(fixture.worker_stack_free[n] != SIZE_MAX && fixture.worker_stack_free[n] >= 2048);
#endif
    }
    printf("capacity deliveries=%llu\n", (unsigned long long)fixture.deliveries);
    pthread_mutex_unlock(&registry);
}

static void remove_all(void)
{
    for (size_t c = 0; c < CHANNELS; ++c) {
        fixture_request request = {.id = next_id(), .scope = {.kind = CORE_REQUEST_REMOVE_EDGE,
            .channel_count = 1, .allowance_us = ALLOWANCE_US}};
        pthread_mutex_lock(&registry);
        const core_admission_record *creator = core_admission_find(&fixture.admission, fixture.edge_ids[c]);
        CHECK(creator != NULL);
        request.scope.bindings[0] = creator->scope.bindings[0];
        (void)submit(&request);
        pthread_mutex_unlock(&registry);
        await_success(request.id);
        pthread_mutex_lock(&registry);
        CHECK(core_admission_forget(&fixture.admission, fixture.edge_ids[c]));
        CHECK(core_admission_forget(&fixture.admission, request.id));
        pthread_mutex_unlock(&registry);
    }
    pthread_mutex_lock(&registry);
    size_t unread = 0;
    for (size_t n = 0; n < NODES; ++n) {
        CHECK(fixture.admission.nodes[n].established);
        for (size_t e = 0; e < CORE_MAX_REQUEST_CHANNELS; ++e)
            unread += fixture.admission.nodes[n].notifications[e].pending;
    }
    CHECK(fixture.admission.reserved_bytes == NODES * fixture.admission.node_bytes);
    printf("capacity endpoints_reclaimed unread_notifications=%zu\n", unread);
    pthread_mutex_unlock(&registry);
    for (size_t n = 0; n < NODES; ++n) {
        fixture_request request = {.id = next_id(), .scope = {.kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES,
            .logical_node_id = 100 + n, .instance = {fixture.node_ids[n].value}, .allowance_us = ALLOWANCE_US}};
        pthread_mutex_lock(&registry);
        (void)submit(&request);
        pthread_mutex_unlock(&registry);
        await_success(request.id);
        pthread_mutex_lock(&registry);
        CHECK(core_admission_forget(&fixture.admission, fixture.node_ids[n]));
        CHECK(core_admission_forget(&fixture.admission, request.id));
        pthread_mutex_unlock(&registry);
    }
    CHECK(fixture.admission.reserved_bytes == 0);
    for (size_t e = 0; e < CORE_MAX_ENDPOINT_RESERVATIONS; ++e)
        CHECK(!fixture.admission.endpoints[e].occupied &&
              core_lifetime_reclaimed(&fixture.admission.endpoints[e].lifetime));
    for (size_t n = 0; n < NODES; ++n) {
        CHECK(!fixture.admission.nodes[n].occupied && core_lifetime_reclaimed(&fixture.admission.nodes[n].lifetime));
        for (size_t e = 0; e < CORE_MAX_REQUEST_CHANNELS; ++e)
            CHECK(!fixture.admission.nodes[n].notifications[e].occupied);
    }
}

static void report_profile_bounds(void)
{
    core_report report;
    CHECK(CORE_REPORT_MAX_OBJECTS == 17);
    CHECK(core_report_init(&report, CORE_REQUEST_ADD_NODE, (core_op_id){UINT64_MAX}, UINT64_MAX, 17));
    CHECK(!core_report_init(&report, CORE_REQUEST_ADD_NODE, (core_op_id){1}, 1, 18));
    const uint8_t histories[] = {CORE_HISTORY_COMMIT, CORE_HISTORY_DISPATCH,
        CORE_HISTORY_STARTED, CORE_HISTORY_SUCCESS, CORE_HISTORY_CLEANUP, CORE_HISTORY_CUTOFF};
    for (size_t h = 0; h < sizeof(histories); ++h)
        CHECK(core_report_record(&report, histories[h]) == CORE_REPORT_CHANGED);
    CHECK(core_report_obligations(&report) == UINT32_C(0x1ffff));
    const uint8_t additions[] = {CORE_OBJECT_READY, CORE_OBJECT_ACTIVE, CORE_OBJECT_FAILED,
        CORE_OBJECT_CLEANUP, CORE_OBJECT_STOPPED, CORE_OBJECT_CLEANED, CORE_OBJECT_EXITED,
        CORE_OBJECT_EXPIRED};
    for (uint8_t object = 0; object < 17; ++object) {
        if (object == 16) CHECK(core_report_obligations(&report) == UINT32_C(0x10000));
        for (size_t f = 0; f < sizeof(additions); ++f)
            CHECK(core_report_observe(&report, object, report.objects[object] | additions[f]) == CORE_REPORT_CHANGED);
    }
    CHECK(report.revision == 142 && core_report_revision_bound(17) == 142);
    CHECK(core_report_obligations(&report) == 0);
    uint8_t encoded[CORE_REPORT_BYTES];
    size_t written;
    core_report decoded;
    CHECK(core_report_encode(&report, encoded, sizeof(encoded), &written));
    CHECK(core_report_decode(encoded, written, CORE_REQUEST_ADD_NODE, &decoded));
    CHECK(decoded.revision == report.revision && decoded.history == report.history &&
          decoded.id.value == UINT64_MAX && decoded.coordinator == UINT64_MAX &&
          !memcmp(decoded.objects, report.objects, sizeof(report.objects)));
}

void p4_capacity_run(const uint8_t *artifact, size_t artifact_size)
{
    report_profile_bounds();
    CHECK(core_wamr_profile(artifact, artifact_size));
#ifdef ESP_PLATFORM
    memory_snapshot("before_runtime");
    changed = xSemaphoreCreateBinaryStatic(&wake_storage);
    CHECK(changed != NULL);
#endif
    CHECK(core_admission_init(&fixture.admission, artifact_size + P4_CAPACITY_ARENA_BYTES,
        NODES * (artifact_size + P4_CAPACITY_ARENA_BYTES) + CORE_MAX_ENDPOINT_RESERVATIONS * CORE_ENDPOINT_BYTES));
    const uint64_t peers[] = {1};
    CHECK(core_admission_configure(&fixture.admission, 1, 99, peers, 1));
    core_endpoint_init(&fixture.endpoints, &fixture.admission, channel_changed, NULL);
    fixture.edges.admission = &fixture.admission;
    fixture.edges.endpoints = &fixture.endpoints;
    fixture.nodes.admission = &fixture.admission;
    CHECK(core_wamr_init(&fixture.nodes.runtime, wake_control, NULL));
#ifdef ESP_PLATFORM
    memory_snapshot("workers_ready");
#endif
    const core_wamr_channels callbacks = {.put = guest_put, .get = guest_get,
        .event_get = guest_event, .event_ready = guest_event_ready};
    CHECK(core_wamr_attach_channels(&callbacks));
#ifdef ESP_PLATFORM
    size_t reclaimed_baseline = 0;
#endif
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        pthread_mutex_lock(&registry);
        fixture.hold_reads = fixture.hold_transfer = true;
        memset(fixture.puts, 0, sizeof(fixture.puts));
        memset(fixture.gets, 0, sizeof(fixture.gets));
        memset(fixture.full, 0, sizeof(fixture.full));
        memset(fixture.validated, 0, sizeof(fixture.validated));
        fixture.deliveries = 0;
#ifdef ESP_PLATFORM
        for (size_t n = 0; n < NODES; ++n) fixture.worker_stack_free[n] = SIZE_MAX;
#endif
        pthread_mutex_unlock(&registry);
        add_nodes(artifact, artifact_size);
        add_edges();
        fill_all_queues();
        exchange_traffic();
        remove_all();
#ifdef ESP_PLATFORM
        for (size_t n = 0; n < NODES; ++n) {
            CHECK(fixture.worker_task[n] != NULL);
            const size_t free_bytes = uxTaskGetStackHighWaterMark(fixture.worker_task[n]);
            if (free_bytes < fixture.worker_stack_free[n]) fixture.worker_stack_free[n] = free_bytes;
            printf("capacity cycle=%u node=%zu after_cleanup_stack_free=%zu\n",
                   cycle, n, fixture.worker_stack_free[n]);
            CHECK(fixture.worker_stack_free[n] >= 2048);
        }
        const size_t reclaimed = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        printf("capacity cycle=%u reclaimed_free=%zu largest=%zu\n", cycle,
               reclaimed, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (cycle == 0) reclaimed_baseline = reclaimed;
        else CHECK(reclaimed == reclaimed_baseline);
#else
        printf("capacity cycle=%u reservations=0\n", cycle);
#endif
    }
    core_wamr_shutdown();
    puts("P4 capacity PASS: four WAMR nodes, eight endpoints each, 32 full queues, copied traffic and full reclamation");
}

#ifndef ESP_PLATFORM
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    CHECK(length > 0 && length <= CORE_MAX_ARTIFACT_BYTES && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *artifact = malloc((size_t)length);
    CHECK(artifact != NULL && fread(artifact, 1, (size_t)length, file) == (size_t)length);
    CHECK(fclose(file) == 0);
    p4_capacity_run(artifact, (size_t)length);
    free(artifact);
    return 0;
}
#endif

#include "admission_fixture.h"
#include "kaos_host.h"
#include <pthread.h>

typedef struct {
    pthread_mutex_t mutex;
    uint64_t time;
    size_t gets, replies, wakes;
    core_host_packet get;
    core_host_packet pending;
    unsigned waits;
    bool drive_loop, pending_ready;
} fake_platform;
static bool clock_now(void *context, uint64_t *now) { *now = ((fake_platform *)context)->time; return true; }
static void lock(void *context) { CHECK(pthread_mutex_lock(&((fake_platform *)context)->mutex) == 0); }
static void unlock(void *context) { CHECK(pthread_mutex_unlock(&((fake_platform *)context)->mutex) == 0); }
static void wake(void *context) { ++((fake_platform *)context)->wakes; }
static uint64_t retry(void *context) { (void)context; return CORE_COAP_ACK_TIMEOUT_US; }
static core_host_event receive_packet(void *context, core_host_class traffic_class, core_host_packet *packet)
{
    fake_platform *f = context;
    if (!f->pending_ready || f->pending.traffic_class != traffic_class) return CORE_HOST_EMPTY;
    *packet = f->pending;
    f->pending_ready = false;
    return CORE_HOST_PACKET;
}
static core_host_event wait_input(void *context, uint64_t deadline)
{
    fake_platform *f = context;
    if (!f->drive_loop) return CORE_HOST_STOP;
    if (f->waits++ == 0) {
        CHECK(deadline == UINT64_MAX);
        f->pending_ready = true;
        return CORE_HOST_WAKE;
    }
    if (f->waits == 2) {
        CHECK(deadline == 110);
        f->time = deadline;
        return CORE_HOST_WAKE;
    }
    CHECK(deadline == UINT64_MAX);
    return CORE_HOST_STOP;
}
static bool send_packet(void *context, core_host_class traffic_class, uint64_t peer,
                        const uint8_t *bytes, size_t size)
{
    fake_platform *f = context;
    /* I/O must run outside the registry lock. */
    CHECK(pthread_mutex_trylock(&f->mutex) == 0);
    CHECK(pthread_mutex_unlock(&f->mutex) == 0);
    CHECK(peer == 99);
    CHECK(traffic_class == CORE_HOST_MANAGEMENT);
    core_coap_message message;
    CHECK(core_coap_decode(bytes, size, &message));
    if (message.code == CORE_COAP_GET) {
        ++f->gets;
        f->get.size = size;
        memcpy(f->get.bytes, bytes, size);
    } else ++f->replies;
    return true;
}
static core_host *start(fake_platform *f)
{
    *f = (fake_platform){.time = 10};
    CHECK(pthread_mutex_init(&f->mutex, NULL) == 0);
    core_host *host = calloc(1, sizeof(*host));
    CHECK(host != NULL);
    const core_host_platform p = {.context = f, .now = clock_now, .lock = lock, .unlock = unlock,
        .wake = wake, .wait = wait_input, .recv = receive_packet, .send = send_packet, .retry_us = retry};
    const core_host_config config = {.local_device = 1, .authority = 99, .peers = {1},
        .peer_count = 1, .node_bytes = 4096, .byte_limit = 8192, .artifact_interval_us = 0};
    CHECK(core_host_init(host, &config, &p));
    return host;
}
static void finish(core_host *h, fake_platform *f)
{
    core_host_destroy(h);
    CHECK(pthread_mutex_destroy(&f->mutex) == 0);
    free(h);
}
/* Inject through the same receive path used by run. A consumed full batch
 * requests an immediate continuation before the next sleeping deadline. */
static bool step(core_host *h, const core_host_packet *packet, uint64_t *next)
{
    fake_platform *f = h->platform.context;
    CHECK(!f->pending_ready);
    f->pending = *packet;
    f->pending_ready = true;
    if (!core_host_step(h, next)) return false;
    return *next != 0 || core_host_step(h, next);
}
static core_host_packet addition(void)
{
    uint8_t body[CORE_MAX_REQUEST_BYTES];
    fixture_request r = fixture_node(100, 10);
    uint8_t artifact[16] = {0};
    r.node.artifact_crc32 = core_artifact_crc32(artifact, sizeof(artifact));
    const size_t size = fixture_encode(&r, body);
    const uint8_t format = 60;
    const core_coap_message request = {.type = CORE_COAP_CON, .code = CORE_COAP_POST,
        .message_id = 42, .token_size = 1, .token = {5}, .option_count = 2,
        .options = {{CORE_COAP_URI_PATH, (const uint8_t *)"graph", 5},
                    {CORE_COAP_CONTENT_FORMAT, &format, 1}}, .payload = body, .payload_size = size};
    core_host_packet packet = {.sender = 99};
    CHECK(core_coap_encode(&request, packet.bytes, sizeof(packet.bytes), &packet.size));
    return packet;
}
static core_host_packet artifact_reply(const core_host_packet *get, bool valid)
{
    core_coap_message request;
    CHECK(core_coap_decode(get->bytes, get->size, &request));
    const uint8_t etag = 1, format = 42, block = 4, size = 16;
    uint8_t artifact[16] = {0};
    core_coap_message response = {.type = CORE_COAP_ACK, .code = CORE_COAP_CONTENT,
        .message_id = request.message_id, .token_size = request.token_size,
        .option_count = valid ? 4 : 0,
        .options = {{CORE_COAP_ETAG, &etag, 1}, {CORE_COAP_CONTENT_FORMAT, &format, 1},
                    {CORE_COAP_BLOCK2, &block, 1}, {CORE_COAP_SIZE2, &size, 1}},
        .payload = artifact, .payload_size = sizeof(artifact)};
    memcpy(response.token, request.token, request.token_size);
    core_host_packet packet = {.sender = 99};
    CHECK(core_coap_encode(&response, packet.bytes, sizeof(packet.bytes), &packet.size));
    return packet;
}
static void reclaimed(core_host *h)
{
    const core_admission_record *r = core_admission_find(&h->admission, (core_op_id){100});
    CHECK(r != NULL && r->deadline_us == 110);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(!(r->evidence.history & CORE_HISTORY_DISPATCH));
    CHECK(h->admission.reserved_bytes == 0 && h->active_staging == -1);
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        CHECK(!h->nodes.executions[i].occupied && !h->staging[i].used);
        CHECK(core_lifetime_reclaimed(&h->admission.nodes[i].lifetime));
    }
}
static void bounded_restart_and_duplicate(void)
{
    fake_platform f;
    core_host *h = start(&f);
    core_host_packet packet = addition();
    uint64_t next;
    CHECK(step(h, &packet, &next) && next == 110 && f.gets == 1);
    CHECK(step(h, &packet, &next) && f.gets == 1 && f.replies == 2);
    const core_host_packet first = f.get;
    packet = artifact_reply(&first, false);
    CHECK(step(h, &packet, &next) && f.gets == 2 && next == 110);
    CHECK(step(h, &packet, &next) && f.gets == 2); /* old reply */
    packet = artifact_reply(&f.get, false);
    CHECK(step(h, &packet, &next) && f.gets == 2);
    reclaimed(h);
    finish(h, &f);
}
static void queued_artifact_at_deadline(void)
{
    fake_platform f;
    core_host *h = start(&f);
    core_host_packet packet = addition();
    uint64_t next;
    CHECK(step(h, &packet, &next));
    packet = artifact_reply(&f.get, true);
    f.time = 110;
    CHECK(step(h, &packet, &next));
    reclaimed(h);
    CHECK(f.gets == 1);
    finish(h, &f);
}
static void run_uses_same_scheduler(void)
{
    fake_platform f;
    core_host *h = start(&f);
    f.drive_loop = true;
    f.pending = addition();
    CHECK(core_host_run(h));
    CHECK(f.waits == 3 && f.gets == 1);
    reclaimed(h);
    core_host_request_stop(h);
    CHECK(core_host_run(h) && f.waits == 3);
    finish(h, &f);
}
int main(void)
{
    bounded_restart_and_duplicate();
    queued_artifact_at_deadline();
    run_uses_same_scheduler();
    puts("shared host: duplicate/restart, strict expiry, cleanup and run/step passed");
}

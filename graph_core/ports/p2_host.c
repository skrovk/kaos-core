#define _POSIX_C_SOURCE 200809L

/* P2 exercise port: real UDP control and artifact transfers, no runtime or
 * endpoint execution. A validated artifact remains Pending for the future
 * P3 loader. The original addition deadline still applies during that wait. */
#include "kaos_artifact.h"
#include "kaos_control.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    core_artifact transfer;
    core_op_id id;
    uint8_t *bytes;
    size_t size;
    core_lifetime *owner;
    bool used, charged, completed, restarted;
} staging_record;

static core_admission admission;
static core_control control = {.admission = &admission};
static staging_record staging[CORE_MAX_NODE_RESERVATIONS];
static uint64_t cleanup_deadlines[CORE_MAX_ADMISSION_RECORDS];
static core_coap_exchange exchange;
static struct sockaddr_in authority_address;
static int udp_socket, active_staging = -1;
static uint32_t next_mid = 1;
static uint64_t next_token = 1;
static bool tokens_exhausted;
static volatile sig_atomic_t running = 1;

static void stop(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static core_admission_record *find(core_op_id id)
{
    const core_admission_record *record = core_admission_find(&admission, id);
    return record == NULL ? NULL : &admission.records[record - admission.records];
}

static bool send_to(const uint8_t *data, size_t size, const struct sockaddr_in *address)
{
    const ssize_t sent = sendto(udp_socket, data, size, 0,
                                (const struct sockaddr *)address, sizeof(*address));
    return sent >= 0 && (size_t)sent == size;
}

static bool send_datagram(const uint8_t *data, size_t size)
{
    return send_to(data, size, &authority_address);
}

static bool allocate_token(uint64_t *out)
{
    if (tokens_exhausted) return false;
    *out = next_token;
    if (next_token == UINT64_MAX) tokens_exhausted = true;
    else ++next_token;
    return true;
}

static void token_bytes(uint64_t value, uint8_t out[8])
{
    for (unsigned i = 0; i < 8; ++i) out[7 - i] = (uint8_t)(value >> (8 * i));
}

static void observe(core_admission_record *record, uint8_t object, uint8_t facts)
{
    core_object_report prior = record->evidence.objects[object];
    facts |= prior.facts;
    if (facts != prior.facts) {
        (void)core_report_observe(&record->evidence, object,
                                  (uint8_t)(prior.revision + 1), facts);
    }
}

static bool node_operation(uint8_t kind)
{
    return kind == CORE_REQUEST_ADD_NODE_NO_EDGES || kind == CORE_REQUEST_ADD_NODE ||
           kind == CORE_REQUEST_REMOVE_NODE_NO_EDGES || kind == CORE_REQUEST_REMOVE_NODE;
}

static void local_cleanup(core_admission_record *record);

static void select_cleanup(core_admission_record *record, uint64_t now, const char *reason)
{
    const size_t index = (size_t)(record - admission.records);
    if (cleanup_deadlines[index] == 0) {
        if (record->endpoint_only) {
            cleanup_deadlines[index] = UINT64_MAX; /* Ki does not run Kc's clock. */
        } else if (record->scope.kind >= CORE_REQUEST_REMOVE_NODE_NO_EDGES) {
            cleanup_deadlines[index] = record->deadline_us;
        } else if (!core_deadline(now, record->scope.cleanup_allowance_us,
                                   &cleanup_deadlines[index])) {
            cleanup_deadlines[index] = now;
        }
        (void)core_report_record(&record->evidence, CORE_HISTORY_CLEANUP);
        (void)core_admission_close_scope(&admission, record->id);
        if (!record->endpoint_only &&
            (record->scope.kind == CORE_REQUEST_REMOVE_NODE_NO_EDGES ||
             record->scope.kind == CORE_REQUEST_REMOVE_NODE)) {
            core_admission_record *creator = find((core_op_id){record->scope.instance.value});
            if (creator != NULL && !(creator->evidence.history & CORE_HISTORY_SUCCESS))
                select_cleanup(creator, now, "pending creation removed");
        }
        if (!record->endpoint_only && !core_before_deadline(now, cleanup_deadlines[index]) &&
            core_report_obligations(&record->evidence) != 0)
            (void)core_report_record(&record->evidence, CORE_HISTORY_CUTOFF);
        printf("CLEANUP %" PRIu64 " %s\n", record->id.value, reason);
        local_cleanup(record);
    }
}

static bool same_binding(const core_binding *a, const core_binding *b)
{
    return a->creating_op.value == b->creating_op.value &&
           a->channel.source_logical == b->channel.source_logical &&
           a->channel.destination_logical == b->channel.destination_logical &&
           a->channel.service == b->channel.service;
}

static void local_cleanup(core_admission_record *record)
{
    const core_request_scope *scope = &record->scope;
    if (!record->endpoint_only && node_operation(scope->kind)) {
        for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
            staging_record *s = &staging[i];
            if (!s->used || s->id.value != scope->instance.value) continue;
            if (active_staging == (int)i) {
                core_coap_exchange_cancel(&exchange);
                active_staging = -1;
            }
            core_artifact_cancel(&s->transfer);
            if (!s->owner->closing || !core_lifetime_unclaimed(s->owner)) continue;
            free(s->bytes);
            s->bytes = NULL;
            if (s->charged && !core_lifetime_uncharge(s->owner, s->size)) continue;
            s->charged = false;
            s->used = false;
        }
    }
    /* This exercise port never issues endpoint workers or allocates their
     * data buffers. Closed zero-allocation endpoint reservations can drain. */
    for (uint8_t i = 0; i < scope->channel_count; ++i) {
        (void)core_admission_release_binding(&admission, &scope->bindings[i]);
    }
    uint8_t object = node_operation(scope->kind) ? 1 : 0;
    for (uint8_t i = 0; i < scope->channel_count; ++i) {
        const core_binding *binding = &scope->bindings[i];
        for (unsigned role = 0; role < 2; ++role, ++object) {
            const uint64_t owner = role == 0 ? binding->channel.source_owner :
                                              binding->channel.destination_owner;
            if (owner != admission.local_device) continue;
            bool present = false;
            for (size_t j = 0; j < CORE_MAX_ENDPOINT_RESERVATIONS; ++j) {
                const core_reserved_endpoint *endpoint = &admission.endpoints[j];
                present |= endpoint->occupied && endpoint->sender == (role == 0) &&
                           same_binding(&endpoint->binding, binding);
            }
            if (!present) observe(record, object, CORE_OBJECT_CLEANUP |
                                                  CORE_OBJECT_QUIESCENT | CORE_OBJECT_CLEANED);
        }
    }
    if (!record->endpoint_only && node_operation(scope->kind)) {
        const core_op_id creation_id = {.value = scope->instance.value};
        core_admission_record *creator = find(creation_id);
        if (creator != NULL && creator->node != NULL) {
            (void)core_admission_release_node(&admission, creation_id);
        }
        bool present = false;
        for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
            present |= admission.nodes[i].occupied &&
                       admission.nodes[i].instance.value == scope->instance.value;
        }
        if (!present) observe(record, 0, CORE_OBJECT_CLEANUP |
                                         CORE_OBJECT_QUIESCENT | CORE_OBJECT_CLEANED);
    }
}

static bool send_artifact_request(staging_record *s, uint64_t now)
{
    uint32_t block;
    uint64_t token, wire_token;
    core_admission_record *record = find(s->id);
    if (record == NULL || next_mid > UINT16_MAX ||
        !core_artifact_next(&s->transfer, now, &block, &token) ||
        !allocate_token(&wire_token)) return false;
    char reference[CORE_MAX_ARTIFACT_REF_BYTES * 2];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < record->request.artifact_ref_size; ++i) {
        reference[i * 2] = hex[record->request.artifact_ref[i] >> 4];
        reference[i * 2 + 1] = hex[record->request.artifact_ref[i] & 15];
    }
    uint8_t block_bytes[4];
    const size_t block_size = core_coap_uint(block, block_bytes);
    core_coap_message message = {.type = CORE_COAP_CON, .code = CORE_COAP_GET,
        .message_id = (uint16_t)next_mid++, .token_size = 8,
        .options = {
            {.number = CORE_COAP_URI_PATH, .value = (const uint8_t *)"artifact", .size = 8},
            {.number = CORE_COAP_URI_PATH, .value = (const uint8_t *)reference,
             .size = (size_t)record->request.artifact_ref_size * 2},
            {.number = CORE_COAP_BLOCK2, .value = block_bytes, .size = block_size}
        }, .option_count = 3};
    token_bytes(wire_token, message.token);
    uint8_t bytes[CORE_COAP_MAX_DATAGRAM];
    size_t size;
    const uint64_t timeout = CORE_COAP_ACK_TIMEOUT_US + (uint64_t)(rand() % 1000001);
    if (!core_coap_encode(&message, bytes, sizeof(bytes), &size) ||
        !core_coap_exchange_start(&exchange, admission.authority, bytes, size, now,
                                   record->deadline_us, timeout)) return false;
    if (send_datagram(bytes, size)) return true;
    core_coap_exchange_cancel(&exchange);
    return false;
}

static void transfer_failure(staging_record *s, uint64_t now, const char *reason)
{
    core_coap_exchange_cancel(&exchange);
    uint64_t token;
    /* One full restart is sufficient for this bounded exercise profile.
     * CoAP already owns packet retransmission; no independent retry timer. */
    if (!s->restarted && allocate_token(&token) &&
        core_artifact_restart(&s->transfer, token, now)) {
        s->restarted = true;
        if (send_artifact_request(s, now)) return;
    }
    core_admission_record *record = find(s->id);
    if (record != NULL) {
        observe(record, 0, CORE_OBJECT_FAILED);
        select_cleanup(record, now, reason);
    }
}

static void start_waiting_artifact(uint64_t now)
{
    if (active_staging >= 0) return;
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *record = &admission.records[i];
        if (!record->occupied || record->endpoint_only || !record->request_present ||
            record->node == NULL || record->cleanup_selected ||
            !core_before_deadline(now, record->deadline_us)) continue;
        bool started = false;
        staging_record *available = NULL;
        size_t index = 0;
        for (size_t j = 0; j < CORE_MAX_NODE_RESERVATIONS; ++j) {
            started |= staging[j].used && staging[j].id.value == record->id.value;
            if (!staging[j].used && available == NULL) {
                available = &staging[j];
                index = j;
            }
        }
        if (started || available == NULL) continue;
        staging_record *s = available;
        s->used = true;
        s->id = record->id;
        s->owner = &record->node->lifetime;
        s->size = record->request.artifact_bytes;
        s->bytes = NULL;
        s->charged = false;
        s->completed = false;
        s->restarted = false;
        if (!core_lifetime_charge(s->owner, s->size)) {
            select_cleanup(record, now, "staging capacity");
            return;
        }
        s->charged = true;
        s->bytes = malloc(s->size);
        uint64_t token;
        if (s->bytes == NULL || !allocate_token(&token) ||
            !core_artifact_begin(&s->transfer, s->owner, s->bytes, s->size, s->size,
                                  record->request.artifact_crc32, token, now, record->deadline_us)) {
            select_cleanup(record, now, "staging allocation");
            return;
        }
        active_staging = (int)index;
        if (!send_artifact_request(s, now)) transfer_failure(s, now, "artifact request");
        return;
    }
}

static void service(uint64_t now)
{
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        core_admission_record *record = &admission.records[i];
        if (!record->occupied) continue;
        if (record->cleanup_selected || (record->node != NULL && record->node->lifetime.closing)) {
            select_cleanup(record, now, "selected");
        } else if (!record->endpoint_only && !core_before_deadline(now, record->deadline_us)) {
            if (record->request_present) observe(record, 0, CORE_OBJECT_EXPIRED);
            select_cleanup(record, now, "addition deadline");
        }
        if (record->cleanup_selected) {
            if (!record->endpoint_only && !core_before_deadline(now, cleanup_deadlines[i]) &&
                core_report_obligations(&record->evidence) != 0) {
                (void)core_report_record(&record->evidence, CORE_HISTORY_CUTOFF);
            }
            local_cleanup(record);
        }
    }
    if (active_staging >= 0) {
        staging_record *s = &staging[active_staging];
        const core_coap_poll_result result = core_coap_exchange_poll(&exchange, now);
        if (result == CORE_COAP_SEND && !send_datagram(exchange.request, exchange.request_size)) {
            transfer_failure(s, now, "artifact transport");
        } else if (result == CORE_COAP_TIMED_OUT) {
            transfer_failure(s, now, "artifact exchange timeout");
        }
    }
    start_waiting_artifact(now);
}

static bool artifact_options(const core_coap_message *message, size_t expected,
                              uint32_t *block, const core_coap_option **etag)
{
    bool seen_etag = false, seen_format = false, seen_block = false, seen_size = false;
    for (size_t i = 0; i < message->option_count; ++i) {
        const uint16_t n = message->options[i].number;
        bool *seen = n == CORE_COAP_ETAG ? &seen_etag :
                     n == CORE_COAP_CONTENT_FORMAT ? &seen_format :
                     n == CORE_COAP_BLOCK2 ? &seen_block :
                     n == CORE_COAP_SIZE2 ? &seen_size : NULL;
        if (seen != NULL) {
            if (*seen) return false;
            *seen = true;
        } else if (n & 1u) return false;
    }
    uint32_t format, size;
    *etag = core_coap_find(message, CORE_COAP_ETAG);
    return message->code == CORE_COAP_CONTENT && *etag != NULL &&
           core_coap_option_uint(core_coap_find(message, CORE_COAP_CONTENT_FORMAT), &format) && format == 42 &&
           core_coap_option_uint(core_coap_find(message, CORE_COAP_BLOCK2), block) &&
           core_coap_option_uint(core_coap_find(message, CORE_COAP_SIZE2), &size) && size == expected;
}

static void receive_artifact(const core_coap_message *message, uint64_t now)
{
    bool acknowledge;
    const core_coap_receive_result response = core_coap_exchange_receive(&exchange,
        admission.authority, message, now, &acknowledge);
    if (acknowledge) {
        core_coap_message ack = {.type = CORE_COAP_ACK, .message_id = message->message_id};
        uint8_t bytes[4];
        size_t size;
        if (core_coap_encode(&ack, bytes, sizeof(bytes), &size)) (void)send_datagram(bytes, size);
    }
    if (active_staging < 0) return;
    staging_record *s = &staging[active_staging];
    if (response == CORE_COAP_RESET) {
        transfer_failure(s, now, "artifact reset");
        return;
    }
    if (response != CORE_COAP_RESPONSE) return;
    uint32_t block;
    const core_coap_option *etag;
    if (!artifact_options(message, s->size, &block, &etag)) {
        transfer_failure(s, now, "artifact response");
        return;
    }
    const core_artifact_result result = core_artifact_receive(&s->transfer,
        s->transfer.token, block, etag->value, etag->size,
        message->payload, message->payload_size, now);
    if (result == CORE_ARTIFACT_MORE) {
        if (!send_artifact_request(s, now)) transfer_failure(s, now, "next artifact block");
    } else if (result == CORE_ARTIFACT_COMPLETE) {
        uint8_t *bytes;
        size_t size;
        if (!core_clock_now(&now) || !core_artifact_take(&s->transfer, now, &bytes, &size)) {
            transfer_failure(s, now, "artifact handoff");
            return;
        }
        s->completed = true;
        printf("ARTIFACT %" PRIu64 " %zu\n", s->id.value, size);
        active_staging = -1;
    } else if (result == CORE_ARTIFACT_INVALID) {
        transfer_failure(s, now, "artifact validation");
    }
}

static int wait_milliseconds(uint64_t now)
{
    uint64_t next = UINT64_MAX;
    if (exchange.active) {
        next = exchange.deadline_us;
        if (!exchange.acknowledged && exchange.next_retry_us < next) next = exchange.next_retry_us;
    }
    for (size_t i = 0; i < CORE_MAX_ADMISSION_RECORDS; ++i) {
        const core_admission_record *r = &admission.records[i];
        if (!r->occupied || r->endpoint_only) continue;
        const uint64_t deadline = r->cleanup_selected ? cleanup_deadlines[i] : r->deadline_us;
        if (core_report_outcome(&r->evidence) == CORE_OUTCOME_PENDING && deadline < next) next = deadline;
    }
    if (next == UINT64_MAX) return -1;
    if (next <= now) return 0;
    const uint64_t milliseconds = (next - now) / 1000 + ((next - now) % 1000 != 0);
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static bool parse_number(const char *text, uint64_t maximum, uint64_t *out)
{
    if (text[0] == '\0') return false;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    char *end;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > maximum) return false;
    *out = (uint64_t)value;
    return true;
}

static bool parse_peer(const char *text, uint64_t *id, uint16_t *port)
{
    const char *colon = strchr(text, ':');
    if (colon == NULL || colon == text || (size_t)(colon - text) > 20) return false;
    char identity[21];
    memcpy(identity, text, (size_t)(colon - text));
    identity[colon - text] = '\0';
    uint64_t number;
    if (!parse_number(identity, UINT64_MAX, id) ||
        !parse_number(colon + 1, UINT16_MAX, &number) || number == 0) return false;
    *port = (uint16_t)number;
    return true;
}

int main(int argc, char **argv)
{
    uint64_t port, authority_port, local_device, authority, now;
    if (argc < 5 || argc > 4 + CORE_MAX_PEERS ||
        !parse_number(argv[1], UINT16_MAX, &port) || port == 0 ||
        !parse_number(argv[2], UINT16_MAX, &authority_port) || authority_port == 0 ||
        !parse_number(argv[3], UINT64_MAX, &local_device) ||
        !parse_number(argv[4], UINT64_MAX, &authority) ||
        port == authority_port || local_device == authority ||
        !core_clock_now(&now)) {
        fprintf(stderr, "usage: %s listen_port authority_port local_device_id authority_id [peer_id:udp_port ...]\n", argv[0]);
        return 2;
    }
    uint64_t peers[CORE_MAX_PEERS] = {local_device};
    uint16_t peer_ports[CORE_MAX_PEERS] = {(uint16_t)port};
    const size_t peer_count = (size_t)argc - 4;
    for (size_t i = 1; i < peer_count; ++i) {
        if (!parse_peer(argv[i + 4], &peers[i], &peer_ports[i]) ||
            peers[i] == authority || peer_ports[i] == authority_port) {
            fprintf(stderr, "invalid peer enrollment\n");
            return 2;
        }
        for (size_t j = 0; j < i; ++j) {
            if (peers[i] == peers[j] || peer_ports[i] == peer_ports[j]) {
                fprintf(stderr, "duplicate peer identity or UDP port\n");
                return 2;
            }
        }
    }
    if (!core_admission_init(&admission, 131072, 270336) ||
        !core_admission_configure(&admission, local_device, authority, peers, peer_count)) return 2;
    srand((unsigned)now ^ (unsigned)getpid());
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    authority_address = (struct sockaddr_in){.sin_family = AF_INET,
        .sin_port = htons((uint16_t)authority_port), .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    const int flags = udp_socket < 0 ? -1 : fcntl(udp_socket, F_GETFL, 0);
    if (flags < 0 || fcntl(udp_socket, F_SETFL, flags | O_NONBLOCK) != 0 ||
        bind(udp_socket, (const struct sockaddr *)&local, sizeof(local)) != 0) {
        perror("UDP bind");
        if (udp_socket >= 0) close(udp_socket);
        return 1;
    }
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0) {
        close(udp_socket);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("LISTENING %" PRIu64 "\n", port);
    while (running) {
        if (!core_clock_now(&now)) break;
        service(now);
        struct pollfd descriptor = {.fd = udp_socket, .events = POLLIN};
        const int ready = poll(&descriptor, 1, wait_milliseconds(now));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        uint8_t bytes[CORE_COAP_MAX_DATAGRAM];
        struct sockaddr_in sender;
        struct iovec vector = {.iov_base = bytes, .iov_len = sizeof(bytes)};
        struct msghdr received = {.msg_name = &sender, .msg_namelen = sizeof(sender),
                                  .msg_iov = &vector, .msg_iovlen = 1};
        const ssize_t size = recvmsg(udp_socket, &received, 0);
        if (size < 0 || (received.msg_flags & MSG_TRUNC) ||
            sender.sin_family != AF_INET ||
            sender.sin_addr.s_addr != authority_address.sin_addr.s_addr || !core_clock_now(&now)) continue;
        uint64_t sender_id = authority;
        bool enrolled = sender.sin_port == authority_address.sin_port;
        for (size_t i = 1; i < peer_count && !enrolled; ++i) {
            if (sender.sin_port == htons(peer_ports[i])) {
                sender_id = peers[i];
                enrolled = true;
            }
        }
        if (!enrolled) continue;
        service(now); /* Decision-time expiry wins even over queued datagrams. */
        core_coap_message message;
        if (!core_coap_decode(bytes, (size_t)size, &message)) continue;
        if (message.type == CORE_COAP_ACK || message.type == CORE_COAP_RST || (message.code >> 5) >= 2) {
            if (sender_id == authority) receive_artifact(&message, now);
        } else {
            uint8_t response[CORE_COAP_MAX_DATAGRAM];
            size_t response_size;
            if (core_control_receive(&control, sender_id, bytes, (size_t)size, now,
                                       response, sizeof(response), &response_size)) {
                (void)send_to(response, response_size, &sender);
            }
        }
    }
    /* Process shutdown is an experiment boundary, not a graph outcome. */
    core_coap_exchange_cancel(&exchange);
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        if (staging[i].used) {
            core_lifetime_close(staging[i].owner);
            core_artifact_cancel(&staging[i].transfer);
            free(staging[i].bytes);
        }
    }
    close(udp_socket);
    return 0;
}

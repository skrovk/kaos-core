#include "admission_fixture.h"
#include "host_udp.h"
#include <errno.h>

#define DECLARE(name) core_admit_result name(core_admission *, uint64_t, const uint8_t *, size_t, uint64_t, const core_admission_record **)
DECLARE(before_core_request_submit);
DECLARE(before_core_endpoint_submit);
DECLARE(before_core_endpoint_cancel);
core_admit_result before_core_node_submit(core_admission *, const uint8_t *, size_t, uint64_t, const core_admission_record **);
bool before_core_host_udp_parse_peer(const char *, core_host_udp_peer *);
static core_admission pool, original, before, after;
static size_t cases;

static core_admit_result submit(bool old, unsigned mode, uint64_t sender,
    const uint8_t *bytes, size_t size, uint64_t now, const core_admission_record **out)
{
    switch (mode) {
    case 0: return old ? before_core_request_submit(&pool, sender, bytes, size, now, out) :
                        core_request_submit(&pool, sender, bytes, size, now, out);
    case 1: return old ? before_core_endpoint_submit(&pool, sender, bytes, size, now, out) :
                        core_endpoint_submit(&pool, sender, bytes, size, now, out);
    case 2: return old ? before_core_endpoint_cancel(&pool, sender, bytes, size, now, out) :
                        core_endpoint_cancel(&pool, sender, bytes, size, now, out);
    default: return old ? before_core_node_submit(&pool, bytes, size, now, out) :
                         core_node_submit(&pool, bytes, size, now, out);
    }
}

static void compare(unsigned mode, uint64_t sender, const uint8_t *bytes, size_t size, uint64_t now)
{
    memcpy(&before, &pool, sizeof(pool));
    const core_admission_record *a = &pool.records[0], *b = a;
    const core_admit_result ra = submit(true, mode, sender, bytes, size, now, &a);
    memcpy(&after, &pool, sizeof(pool));
    memcpy(&pool, &before, sizeof(pool));
    const core_admit_result rb = submit(false, mode, sender, bytes, size, now, &b);
    if (ra != rb || a != b || memcmp(&pool, &after, sizeof(pool))) {
        fprintf(stderr, "equivalence failed case=%zu mode=%u size=%zu old=%d new=%d\n",
                cases, mode, size, ra, rb);
        exit(1);
    }
    ++cases;
}

int main(void)
{
    uint8_t wire[CORE_MAX_REQUEST_BYTES], mutated[CORE_MAX_REQUEST_BYTES];
    const uint8_t values[] = {0, 1, 2, 3, 4, 6, 7, 23, 24, 25, 26, 27, 31, 32, 60, 99, 127, 128, 255};
    for (unsigned setup = 0; setup < 4; ++setup) {
        fixture_pool(&pool, 1);
        if (setup > 0) {
            fixture_request node = fixture_node(1, 11);
            size_t size = fixture_encode(&node, wire);
            compare(0, 99, wire, size, 0);
        }
        if (setup == 2) pool.byte_limit = pool.reserved_bytes;
        if (setup == 3) {
            for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i)
                pool.nodes[i].lifetime.last_ticket = UINT64_MAX;
        }
        memcpy(&original, &pool, sizeof(pool));
        for (uint8_t kind = CORE_REQUEST_ADD_NODE_NO_EDGES; kind <= CORE_REQUEST_REMOVE_EDGE; ++kind) {
            fixture_request request = fixture_node(42, 42);
            request.scope.kind = kind;
            request.scope.logical_node_id = 11;
            request.scope.instance.value = 1;
            request.scope.allowance_us = 100;
            request.scope.cleanup_allowance_us = 50;
            if (kind == CORE_REQUEST_ADD_NODE || kind == CORE_REQUEST_REMOVE_NODE ||
                kind == CORE_REQUEST_ADD_EDGE || kind == CORE_REQUEST_REMOVE_EDGE) {
                request.scope.channel_count = 1;
                request.scope.bindings[0] = (core_binding){.creating_op = {10},
                    .channel = fixture_channel(42, 11, 42, 1, 2, 1)};
                const core_predicate_token always = {.kind = CORE_PREDICATE_TRUE};
                CHECK(core_predicate_init(&request.scope.predicate, &always, 1, 1));
            }
            const size_t size = fixture_encode(&request, wire);
            for (unsigned mode = 0; mode < 4; ++mode) {
                const uint64_t senders[] = {99, 2, 98};
                for (size_t sender = 0; sender < 3; ++sender) {
                    for (size_t n = 0; n <= size; ++n) {
                        memcpy(&pool, &original, sizeof(pool));
                        compare(mode, senders[sender], wire, n, 10);
                    }
                    for (size_t i = 0; i < size; ++i) {
                        for (size_t v = 0; v < sizeof(values); ++v) {
                            memcpy(mutated, wire, size); mutated[i] = values[v];
                            memcpy(&pool, &original, sizeof(pool));
                            compare(mode, senders[sender], mutated, size, 10);
                        }
                    }
                    memcpy(&pool, &original, sizeof(pool));
                    compare(mode, senders[sender], wire, size, UINT64_MAX - 1);
                }
            }
        }
    }
    printf("request/admission old-new comparisons: %zu\n", cases);
    const char *ids[] = {"", "0", "1", "18446744073709551615", "18446744073709551616", "1@127.0.0.1", "1@bad", "1@0.0.0.0"};
    const char *ports[] = {"", "0", "1", "65535", "65536", "00001", "+1", "-1", " 1", "1 ", "a", "1:2", "999999999999999999999999"};
    size_t peers = 0;
    for (size_t i = 0; i < sizeof(ids)/sizeof(ids[0]); ++i)
        for (size_t m = 0; m < sizeof(ports)/sizeof(ports[0]); ++m)
            for (size_t a = 0; a <= sizeof(ports)/sizeof(ports[0]); ++a) {
                char text[160];
                snprintf(text, sizeof(text), "%s:%s%s%s", ids[i], ports[m], a ? ":" : "", a ? ports[a-1] : "");
                core_host_udp_peer before_peer, after_peer;
                memset(&before_peer, 0xa5, sizeof(before_peer));
                memset(&after_peer, 0xa5, sizeof(after_peer));
                errno = EDOM;
                const bool old = before_core_host_udp_parse_peer(text, &before_peer);
                const int old_errno = errno;
                errno = EDOM;
                const bool current = core_host_udp_parse_peer(text, &after_peer);
                CHECK(old == current && old_errno == errno);
                CHECK(memcmp(&before_peer, &after_peer, sizeof(before_peer)) == 0);
                ++peers;
            }
    printf("UDP parser old-new comparisons: %zu\n", peers);
    return 0;
}

#ifndef KAOS_COAP_H
#define KAOS_COAP_H

#include "kaos_graph.h"

/* Small RFC 7252/7959 UDP profile. No sockets, allocation, clock reads or
 * callbacks. Calls and borrowed message views are serialized by the caller.
 * Datagrams, complete control bodies and block size have independent limits. */
#define CORE_COAP_MAX_DATAGRAM 1152
#define CORE_COAP_MAX_OPTIONS 16
#define CORE_COAP_MAX_BODY 2048
#define CORE_COAP_CACHE_SIZE 4
#define CORE_COAP_EXCHANGE_LIFETIME_US UINT64_C(247000000)
#define CORE_COAP_ACK_TIMEOUT_US UINT64_C(2000000)
#define CORE_COAP_MAX_RETRANSMIT 4

enum { CORE_COAP_CON, CORE_COAP_NON, CORE_COAP_ACK, CORE_COAP_RST };
enum {
    CORE_COAP_GET = 1, CORE_COAP_POST = 2,
    CORE_COAP_CREATED = 65, CORE_COAP_CHANGED = 68,
    CORE_COAP_CONTENT = 69, CORE_COAP_CONTINUE = 95,
    CORE_COAP_BAD_REQUEST = 128, CORE_COAP_UNAUTHORIZED = 129,
    CORE_COAP_BAD_OPTION = 130, CORE_COAP_NOT_FOUND = 132,
    CORE_COAP_INCOMPLETE = 136, CORE_COAP_TOO_LARGE = 141,
    CORE_COAP_UNAVAILABLE = 163
};
enum {
    CORE_COAP_ETAG = 4, CORE_COAP_URI_PATH = 11,
    CORE_COAP_CONTENT_FORMAT = 12, CORE_COAP_BLOCK2 = 23,
    CORE_COAP_BLOCK1 = 27, CORE_COAP_SIZE2 = 28,
    CORE_COAP_SIZE1 = 60, CORE_COAP_REQUEST_TAG = 292
};

typedef struct {
    uint16_t number;
    const uint8_t *value;
    size_t size;
} core_coap_option;

typedef struct {
    uint8_t type, code, token_size;
    uint16_t message_id;
    uint8_t token[8];
    core_coap_option options[CORE_COAP_MAX_OPTIONS];
    size_t option_count;
    const uint8_t *payload;
    size_t payload_size;
} core_coap_message;

/* Decode is structural; the resource handler checks supported/critical options
 * and option multiplicity. Views borrow the original datagram; keep its bytes
 * stable while inspecting the decoded message.
 * Encode requires ascending option numbers (equal numbers are allowed).
 * Failure leaves out_size/out_message unchanged; encode may touch output bytes. */
bool core_coap_decode(const uint8_t *data, size_t size, core_coap_message *out);
bool core_coap_encode(const core_coap_message *message, uint8_t *out,
                      size_t capacity, size_t *out_size);
/* NULL also means repeated option: use this only for singleton options. */
const core_coap_option *core_coap_find(const core_coap_message *message,
                                      uint16_t number);
bool core_coap_option_uint(const core_coap_option *option, uint32_t *out);
/* RFC integer option encoding; zero has length zero. */
size_t core_coap_uint(uint32_t value, uint8_t out[4]);

typedef struct {
    uint8_t request[CORE_COAP_MAX_DATAGRAM];
    size_t request_size;
    uint64_t peer, deadline_us, next_retry_us, retry_interval_us;
    uint16_t message_id;
    uint8_t token[8], token_size, retransmissions;
    bool active, acknowledged;
} core_coap_exchange;

typedef enum {
    CORE_COAP_WAIT, CORE_COAP_SEND, CORE_COAP_TIMED_OUT
} core_coap_poll_result;
typedef enum {
    CORE_COAP_IGNORED, CORE_COAP_ACKNOWLEDGED,
    CORE_COAP_RESPONSE, CORE_COAP_RESET
} core_coap_receive_result;

/* Initializes an inactive caller-owned exchange and copies a CON request.
 * Caller sends the request once after success. initial_timeout_us is a random
 * value in [2,3] seconds supplied by the platform (injectable in tests). MID and
 * token allocation belong to the caller; do not reuse a MID at one peer within
 * EXCHANGE_LIFETIME, nor a token across abandoned artifact attempts.
 * One active exchange per peer is the default NSTART=1 profile. */
bool core_coap_exchange_start(core_coap_exchange *exchange, uint64_t peer,
                              const uint8_t *request, size_t request_size,
                              uint64_t now_us, uint64_t deadline_us,
                              uint64_t initial_timeout_us);
/* SEND borrows exchange.request, unchanged from the first transmission.
 * An empty ACK stops retransmission; response waiting keeps the same deadline. */
core_coap_poll_result core_coap_exchange_poll(core_coap_exchange *exchange,
                                             uint64_t now_us);
/* A valid CON response needs an empty ACK even when its token is stale. The
 * caller emits it when *acknowledge is true; its MID is message.message_id.
 * Accepted response content is still borrowed from the input message. */
core_coap_receive_result core_coap_exchange_receive(core_coap_exchange *exchange,
                                                   uint64_t peer,
                                                   const core_coap_message *message,
                                                   uint64_t now_us,
                                                   bool *acknowledge);
void core_coap_exchange_cancel(core_coap_exchange *exchange);

typedef struct {
    uint64_t peer, expires_us;
    uint16_t message_id;
    uint8_t response[CORE_COAP_MAX_DATAGRAM];
    size_t response_size;
    bool occupied;
} core_coap_cached_response;
typedef struct {
    core_coap_cached_response entries[CORE_COAP_CACHE_SIZE];
} core_coap_cache;
typedef enum { CORE_COAP_CACHE_NEW, CORE_COAP_CACHE_DUPLICATE,
               CORE_COAP_CACHE_FULL } core_coap_cache_result;

/* Zero initialize once. Reserve BEFORE a non-idempotent handler runs, then
 * store its response without an intervening event. Duplicates replay this
 * response. Unexpired entries are never evicted. A full cache must reject new
 * managed work; a bounded 5.03 response needs no application reservation. */
core_coap_cache_result core_coap_cache_reserve(core_coap_cache *cache,
                                               uint64_t peer, uint16_t message_id,
                                               uint64_t now_us,
                                               core_coap_cached_response **out);
bool core_coap_cache_store(core_coap_cached_response *entry,
                           const uint8_t *response, size_t response_size);

/* One bounded multipart request workspace per peer/control resource. The
 * caller requires a nonempty Request-Tag for multipart requests and supplies
 * it unchanged for every block; CoAP tokens may differ between blocks.
 * Caller expires/resets incomplete workspace at a fixed reception deadline;
 * retries must not extend it. No managed-object work precedes COMPLETE. */
typedef struct {
    uint8_t body[CORE_COAP_MAX_BODY];
    uint8_t tag[8], tag_size, szx;
    uint64_t peer;
    size_t size, declared_size;
    bool active, complete;
} core_coap_block1;
typedef enum {
    CORE_COAP_BLOCK_INVALID, CORE_COAP_BLOCK_MORE,
    CORE_COAP_BLOCK_DUPLICATE, CORE_COAP_BLOCK_COMPLETE
} core_coap_block_result;
core_coap_block_result core_coap_block1_receive(core_coap_block1 *assembly,
                                               uint64_t peer,
                                               const uint8_t *tag, size_t tag_size,
                                               uint32_t block, size_t declared_size,
                                               const uint8_t *payload,
                                               size_t payload_size);
void core_coap_block1_reset(core_coap_block1 *assembly);

#endif

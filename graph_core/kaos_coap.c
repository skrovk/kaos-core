#include "kaos_coap.h"

#include <string.h>

static bool code_valid(uint8_t type, uint8_t code)
{
    const uint8_t class_id = code >> 5;
    if (type > CORE_COAP_RST || (type == CORE_COAP_RST && code != 0)) {
        return false;
    }
    if (code == 0) {
        return true;
    }
    if (class_id == 0) {
        return type == CORE_COAP_CON || type == CORE_COAP_NON;
    }
    return class_id == 2 || class_id == 4 || class_id == 5;
}

static bool decode_argument(const uint8_t *data, size_t size, size_t *offset,
                            uint8_t nibble, uint32_t *out)
{
    if (nibble < 13) {
        *out = nibble;
        return true;
    }
    const size_t count = nibble == 13 ? 1 : 2;
    if (nibble == 15 || count > size - *offset) {
        return false;
    }
    uint32_t value = data[(*offset)++];
    if (count == 2) {
        value = (value << 8) | data[(*offset)++];
    }
    *out = value + (nibble == 13 ? 13 : 269);
    return true;
}

bool core_coap_decode(const uint8_t *data, size_t size, core_coap_message *out)
{
    if (data == NULL || size < 4 || size > CORE_COAP_MAX_DATAGRAM ||
        (data[0] >> 6) != 1) {
        return false;
    }
    core_coap_message message = {
        .type = (data[0] >> 4) & 3,
        .code = data[1], .token_size = data[0] & 15,
        .message_id = (uint16_t)((uint16_t)data[2] << 8) | data[3]
    };
    if (message.token_size > 8 || message.token_size > size - 4 ||
        !code_valid(message.type, message.code) ||
        (message.code == 0 && size != 4)) {
        return false;
    }
    memcpy(message.token, data + 4, message.token_size);
    size_t offset = 4 + message.token_size;
    uint32_t option_number = 0;
    while (offset < size && data[offset] != 0xff) {
        if (message.option_count == CORE_COAP_MAX_OPTIONS) {
            return false;
        }
        const uint8_t header = data[offset++];
        uint32_t delta, length;
        if (!decode_argument(data, size, &offset, header >> 4, &delta) ||
            !decode_argument(data, size, &offset, header & 15, &length) ||
            delta > UINT16_MAX - option_number || length > size - offset) {
            return false;
        }
        option_number += delta;
        message.options[message.option_count++] = (core_coap_option){
            .number = (uint16_t)option_number,
            .value = data + offset, .size = length
        };
        offset += length;
    }
    if (offset < size) {
        ++offset;
        if (offset == size) {
            return false; /* A payload marker must be followed by payload. */
        }
        message.payload = data + offset;
        message.payload_size = size - offset;
    }
    *out = message;
    return true;
}

static uint8_t argument_nibble(size_t value)
{
    return value < 13 ? (uint8_t)value : value < 269 ? 13 : 14;
}

static size_t argument_size(size_t value)
{
    return value < 13 ? 0 : value < 269 ? 1 : 2;
}

static void encode_argument(uint8_t *out, size_t *offset, size_t value)
{
    if (value < 13) {
        return;
    }
    if (value < 269) {
        out[(*offset)++] = (uint8_t)(value - 13);
        return;
    }
    value -= 269;
    out[(*offset)++] = (uint8_t)(value >> 8);
    out[(*offset)++] = (uint8_t)value;
}

bool core_coap_encode(const core_coap_message *message, uint8_t *out,
                      size_t capacity, size_t *out_size)
{
    if (out == NULL || !code_valid(message->type, message->code) ||
        message->token_size > 8 || message->option_count > CORE_COAP_MAX_OPTIONS ||
        message->payload_size > CORE_COAP_MAX_DATAGRAM ||
        (message->payload_size != 0 && message->payload == NULL) ||
        (message->code == 0 && (message->token_size != 0 ||
                              message->option_count != 0 ||
                              message->payload_size != 0))) {
        return false;
    }
    size_t required = 4 + message->token_size;
    uint16_t previous = 0;
    for (size_t i = 0; i < message->option_count; ++i) {
        const core_coap_option *option = &message->options[i];
        if (option->number < previous || option->size > CORE_COAP_MAX_DATAGRAM ||
            (option->size != 0 && option->value == NULL)) {
            return false;
        }
        required += 1 + argument_size(option->number - previous) +
                    argument_size(option->size) + option->size;
        previous = option->number;
    }
    required += message->payload_size + (message->payload_size != 0 ? 1 : 0);
    if (required > capacity || required > CORE_COAP_MAX_DATAGRAM) {
        return false;
    }
    out[0] = (uint8_t)(0x40 | (message->type << 4) | message->token_size);
    out[1] = message->code;
    out[2] = (uint8_t)(message->message_id >> 8);
    out[3] = (uint8_t)message->message_id;
    memcpy(out + 4, message->token, message->token_size);
    size_t offset = 4 + message->token_size;
    previous = 0;
    for (size_t i = 0; i < message->option_count; ++i) {
        const core_coap_option *option = &message->options[i];
        const size_t delta = option->number - previous;
        out[offset++] = (uint8_t)((argument_nibble(delta) << 4) |
                                 argument_nibble(option->size));
        encode_argument(out, &offset, delta);
        encode_argument(out, &offset, option->size);
        if (option->size != 0) {
            memcpy(out + offset, option->value, option->size);
            offset += option->size;
        }
        previous = option->number;
    }
    if (message->payload_size != 0) {
        out[offset++] = 0xff;
        memcpy(out + offset, message->payload, message->payload_size);
        offset += message->payload_size;
    }
    *out_size = offset;
    return true;
}

const core_coap_option *core_coap_find(const core_coap_message *message,
                                      uint16_t number)
{
    const core_coap_option *found = NULL;
    for (size_t i = 0; i < message->option_count; ++i) {
        if (message->options[i].number == number) {
            if (found != NULL) {
                return NULL;
            }
            found = &message->options[i];
        }
    }
    return found;
}

bool core_coap_option_uint(const core_coap_option *option, uint32_t *out)
{
    if (option == NULL || option->size > 4 ||
        (option->size != 0 && option->value == NULL)) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < option->size; ++i) {
        value = (value << 8) | option->value[i];
    }
    *out = value;
    return true;
}

size_t core_coap_uint(uint32_t value, uint8_t out[4])
{
    size_t size = 0;
    uint32_t remaining = value;
    while (remaining != 0) {
        ++size;
        remaining >>= 8;
    }
    for (size_t i = 0; i < size; ++i) {
        out[size - 1 - i] = (uint8_t)(value >> (8 * i));
    }
    return size;
}

static uint64_t retry_time(uint64_t now, uint64_t interval, uint64_t deadline)
{
    return interval >= deadline - now ? deadline : now + interval;
}

bool core_coap_exchange_start(core_coap_exchange *exchange, uint64_t peer,
                              const uint8_t *request, size_t request_size,
                              uint64_t now_us, uint64_t deadline_us,
                              uint64_t initial_timeout_us)
{
    core_coap_message message;
    if (exchange->active || !core_before_deadline(now_us, deadline_us) ||
        initial_timeout_us < CORE_COAP_ACK_TIMEOUT_US ||
        initial_timeout_us > CORE_COAP_ACK_TIMEOUT_US * 3 / 2 ||
        !core_coap_decode(request, request_size, &message) ||
        message.type != CORE_COAP_CON || message.code == 0 ||
        (message.code >> 5) != 0) {
        return false;
    }
    /* Validation is complete. Publish directly instead of making a second
     * datagram-sized record on the embedded stack. memmove permits restarting
     * from bytes already held in an inactive exchange's request buffer. */
    memmove(exchange->request, request, request_size);
    exchange->request_size = request_size;
    exchange->peer = peer;
    exchange->deadline_us = deadline_us;
    exchange->next_retry_us = retry_time(now_us, initial_timeout_us, deadline_us);
    exchange->retry_interval_us = initial_timeout_us;
    exchange->message_id = message.message_id;
    exchange->token_size = message.token_size;
    memset(exchange->token, 0, sizeof(exchange->token));
    memcpy(exchange->token, message.token, message.token_size);
    exchange->retransmissions = 0;
    exchange->acknowledged = false;
    exchange->active = true;
    return true;
}

core_coap_poll_result core_coap_exchange_poll(core_coap_exchange *exchange,
                                             uint64_t now_us)
{
    if (!exchange->active) {
        return CORE_COAP_WAIT;
    }
    if (!core_before_deadline(now_us, exchange->deadline_us)) {
        exchange->active = false;
        return CORE_COAP_TIMED_OUT;
    }
    if (exchange->acknowledged || now_us < exchange->next_retry_us) {
        return CORE_COAP_WAIT;
    }
    if (exchange->retransmissions == CORE_COAP_MAX_RETRANSMIT) {
        exchange->active = false;
        return CORE_COAP_TIMED_OUT;
    }
    ++exchange->retransmissions;
    exchange->retry_interval_us *= 2;
    exchange->next_retry_us = retry_time(now_us, exchange->retry_interval_us,
                                        exchange->deadline_us);
    return CORE_COAP_SEND;
}

core_coap_receive_result core_coap_exchange_receive(core_coap_exchange *exchange,
                                                   uint64_t peer,
                                                   const core_coap_message *message,
                                                   uint64_t now_us,
                                                   bool *acknowledge)
{
    *acknowledge = peer == exchange->peer && message->type == CORE_COAP_CON &&
                   (message->code >> 5) >= 2;
    if (peer != exchange->peer || !exchange->active) {
        return CORE_COAP_IGNORED;
    }
    if (!core_before_deadline(now_us, exchange->deadline_us)) {
        exchange->active = false;
        return CORE_COAP_IGNORED;
    }
    if (message->type == CORE_COAP_RST) {
        if (message->code != 0 || message->message_id != exchange->message_id) {
            return CORE_COAP_IGNORED;
        }
        exchange->active = false;
        return CORE_COAP_RESET;
    }
    if (message->type == CORE_COAP_ACK) {
        if (message->message_id != exchange->message_id) {
            return CORE_COAP_IGNORED;
        }
        if (message->code == 0) {
            exchange->acknowledged = true;
            return CORE_COAP_ACKNOWLEDGED;
        }
    }
    if ((message->code >> 5) < 2 || message->token_size != exchange->token_size ||
        memcmp(message->token, exchange->token, exchange->token_size) != 0) {
        return CORE_COAP_IGNORED;
    }
    exchange->active = false;
    return CORE_COAP_RESPONSE;
}

void core_coap_exchange_cancel(core_coap_exchange *exchange)
{
    exchange->active = false;
}

core_coap_cache_result core_coap_cache_reserve(core_coap_cache *cache,
                                               uint64_t peer, uint16_t message_id,
                                               uint64_t now_us,
                                               core_coap_cached_response **out)
{
    core_coap_cached_response *available = NULL;
    for (size_t i = 0; i < CORE_COAP_CACHE_SIZE; ++i) {
        core_coap_cached_response *entry = &cache->entries[i];
        if (entry->occupied && now_us >= entry->expires_us) {
            entry->occupied = false;
        }
        if (entry->occupied && entry->peer == peer && entry->message_id == message_id) {
            *out = entry;
            return CORE_COAP_CACHE_DUPLICATE;
        }
        if (!entry->occupied && available == NULL) {
            available = entry;
        }
    }
    uint64_t expiry;
    if (available == NULL || !core_deadline(now_us, CORE_COAP_EXCHANGE_LIFETIME_US,
                                           &expiry)) {
        return CORE_COAP_CACHE_FULL;
    }
    *available = (core_coap_cached_response){
        .peer = peer, .message_id = message_id, .expires_us = expiry,
        .occupied = true
    };
    *out = available;
    return CORE_COAP_CACHE_NEW;
}

bool core_coap_cache_store(core_coap_cached_response *entry,
                           const uint8_t *response, size_t response_size)
{
    core_coap_message decoded;
    if (!entry->occupied || entry->response_size != 0 ||
        !core_coap_decode(response, response_size, &decoded) ||
        decoded.message_id != entry->message_id ||
        (decoded.type != CORE_COAP_ACK && decoded.type != CORE_COAP_RST)) {
        return false;
    }
    memcpy(entry->response, response, response_size);
    entry->response_size = response_size;
    return true;
}

core_coap_block_result core_coap_block1_receive(core_coap_block1 *assembly,
                                               uint64_t peer,
                                               const uint8_t *tag, size_t tag_size,
                                               uint32_t block, size_t declared_size,
                                               const uint8_t *payload,
                                               size_t payload_size)
{
    const uint8_t szx = block & 7;
    const bool more = (block & 8) != 0;
    if (block > UINT32_C(0xffffff) || szx > 6 || tag == NULL || tag_size == 0 ||
        tag_size > 8 || declared_size > CORE_COAP_MAX_BODY || payload == NULL) {
        return CORE_COAP_BLOCK_INVALID;
    }
    const size_t block_size = (size_t)1 << (szx + 4);
    const size_t offset = (size_t)(block >> 4) * block_size;
    if (payload_size > block_size || payload_size == 0 ||
        (more && payload_size != block_size) || offset > CORE_COAP_MAX_BODY ||
        payload_size > CORE_COAP_MAX_BODY - offset ||
        (more && offset + payload_size == CORE_COAP_MAX_BODY)) {
        return CORE_COAP_BLOCK_INVALID;
    }
    if (!assembly->active) {
        if (offset != 0 || (declared_size != 0 &&
            (offset + payload_size > declared_size ||
             (!more && payload_size != declared_size) ||
             (more && payload_size == declared_size)))) {
            return CORE_COAP_BLOCK_INVALID;
        }
        assembly->peer = peer;
        assembly->tag_size = (uint8_t)tag_size;
        memcpy(assembly->tag, tag, tag_size);
        assembly->szx = szx;
        assembly->size = 0;
        assembly->declared_size = declared_size;
        assembly->complete = false;
        assembly->active = true;
    } else if (assembly->peer != peer || assembly->tag_size != tag_size ||
               memcmp(assembly->tag, tag, tag_size) != 0 || assembly->szx != szx ||
               (declared_size != 0 && declared_size != assembly->declared_size)) {
        return CORE_COAP_BLOCK_INVALID;
    }
    const size_t end = offset + payload_size;
    if (offset < assembly->size) {
        if (end > assembly->size ||
            memcmp(assembly->body + offset, payload, payload_size) != 0 ||
            (more && end == assembly->size && assembly->complete) ||
            (!more && (!assembly->complete || end != assembly->size))) {
            return CORE_COAP_BLOCK_INVALID;
        }
        return CORE_COAP_BLOCK_DUPLICATE;
    }
    if (assembly->complete || offset != assembly->size ||
        (assembly->declared_size != 0 &&
         (end > assembly->declared_size ||
          (!more && end != assembly->declared_size) ||
          (more && end == assembly->declared_size)))) {
        return CORE_COAP_BLOCK_INVALID;
    }
    memcpy(assembly->body + offset, payload, payload_size);
    assembly->size = end;
    assembly->complete = !more;
    return more ? CORE_COAP_BLOCK_MORE : CORE_COAP_BLOCK_COMPLETE;
}

void core_coap_block1_reset(core_coap_block1 *assembly)
{
    assembly->active = false;
    assembly->complete = false;
    assembly->size = 0;
}

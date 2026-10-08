#ifndef KAOS_CBOR_H
#define KAOS_CBOR_H

/* Private fixed-shape deterministic CBOR primitives. No allocation, recursion,
 * indefinite items, maps, tags or floating point. Callers check exact shapes,
 * bounds and complete consumption before publishing decoded state. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} core_cbor_reader;

static inline bool core_cbor_head(core_cbor_reader *r, uint8_t type, uint64_t *out)
{
    if (r->offset >= r->size) return false;
    const uint8_t head = r->data[r->offset++];
    const uint8_t arg = head & 31u;
    if ((head >> 5) != type || arg > 27) return false;
    if (arg < 24) { *out = arg; return true; }
    const size_t bytes = (size_t)1 << (arg - 24);
    if (bytes > r->size - r->offset) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value = (value << 8) | r->data[r->offset++];
    static const uint64_t minimum[] = {24, 256, 65536, UINT64_C(4294967296)};
    if (value < minimum[arg - 24]) return false;
    *out = value;
    return true;
}

static inline bool core_cbor_uint(core_cbor_reader *r, uint64_t *out)
{ return core_cbor_head(r, 0, out); }

static inline bool core_cbor_array(core_cbor_reader *r, uint64_t *count)
{ return core_cbor_head(r, 4, count); }

static inline bool core_cbor_bytes(core_cbor_reader *r, const uint8_t **out, size_t *size)
{
    uint64_t length;
    if (!core_cbor_head(r, 2, &length) || length > r->size - r->offset) return false;
    *out = r->data + r->offset;
    *size = (size_t)length;
    r->offset += *size;
    return true;
}

typedef struct {
    uint8_t *data;
    size_t size;
    size_t offset;
    bool failed;
} core_cbor_writer;

static inline bool core_cbor_write_head(core_cbor_writer *w, uint8_t type, uint64_t value)
{
    const size_t bytes = value < 24 ? 0 : value <= UINT8_MAX ? 1 :
        value <= UINT16_MAX ? 2 : value <= UINT32_MAX ? 4 : 8;
    if (w->failed || w->offset > w->size || bytes + 1 > w->size - w->offset) {
        w->failed = true;
        return false;
    }
    const uint8_t arg = bytes == 0 ? (uint8_t)value : bytes == 1 ? 24 :
        bytes == 2 ? 25 : bytes == 4 ? 26 : 27;
    w->data[w->offset++] = (uint8_t)((type << 5) | arg);
    for (size_t i = bytes; i > 0; --i)
        w->data[w->offset++] = (uint8_t)(value >> ((i - 1) * 8));
    return true;
}

static inline bool core_cbor_write_uint(core_cbor_writer *w, uint64_t value)
{ return core_cbor_write_head(w, 0, value); }

static inline bool core_cbor_write_array(core_cbor_writer *w, size_t count)
{ return core_cbor_write_head(w, 4, count); }

static inline bool core_cbor_write_bytes(core_cbor_writer *w, const uint8_t *data, size_t size)
{
    if (!core_cbor_write_head(w, 2, size)) return false;
    if (size > w->size - w->offset) { w->failed = true; return false; }
    if (size != 0) memcpy(w->data + w->offset, data, size);
    w->offset += size;
    return true;
}

#endif

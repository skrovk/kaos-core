#include "kaos_wamr.h"
#include "kaos_admission.h"
#include "native_internal.h"

#include <string.h>

typedef struct { const uint8_t *at, *end; } reader;

static bool u32(reader *r, uint32_t *out)
{
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (r->at == r->end) return false;
        const uint8_t byte = *r->at++;
        if (shift == 28 && (byte & 0xf0)) return false;
        value |= (uint32_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) { *out = value; return true; }
    }
    return false;
}

static bool name(reader *r, reader *out)
{
    uint32_t size;
    if (!u32(r, &size) || size > (size_t)(r->end - r->at)) return false;
    *out = (reader){r->at, r->at + size};
    r->at += size;
    return true;
}

static bool equals(reader r, const char *text)
{
    return (size_t)(r.end - r.at) == strlen(text) &&
           memcmp(r.at, text, strlen(text)) == 0;
}

static bool allowed(reader module, reader field, const core_native_imports *imports)
{
    if (equals(module, "kaos"))
        return equals(field, "init") || equals(field, "error") ||
               equals(field, "event_wait") || equals(field, "put") ||
               equals(field, "get") || equals(field, "event_get");
    if (!equals(module, CORE_NATIVE_MODULE)) return false;
    for (size_t i = 0; i < imports->count; ++i)
        if (equals(field, imports->names[i])) return true;
    return false;
}

bool core_wamr_profile_with_imports(const uint8_t *bytes, size_t size,
                                  const core_native_imports *imports)
{
    static const uint8_t magic[] = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
    if (!core_native_grants_registered(imports) ||
        !bytes || size < sizeof(magic) || size > CORE_MAX_ARTIFACT_BYTES ||
        memcmp(bytes, magic, sizeof(magic)) != 0) return false;
    reader input = {bytes + sizeof(magic), bytes + size};
    bool memory = false, configure = false, main = false;
    while (input.at < input.end) {
        const uint8_t section = *input.at++;
        uint32_t size, count;
        if (section > 12 || !u32(&input, &size) ||
            size > (size_t)(input.end - input.at)) return false;
        reader r = {input.at, input.at + size};
        input.at += size;
        if (section == 8) return false; /* implicit start always forbidden */
        if (section == 2) {
            if (!u32(&r, &count) || count > 6 + CORE_MAX_NATIVE_IMPORTS) return false;
            for (uint32_t i = 0; i < count; ++i) {
                reader module, field;
                uint32_t type;
                if (!name(&r, &module) || !name(&r, &field) || r.at == r.end ||
                    *r.at++ != 0 || !u32(&r, &type) ||
                    !allowed(module, field, imports)) return false;
            }
            if (r.at != r.end) return false;
        } else if (section == 4) {
            if (!u32(&r, &count) || count != 0 || r.at != r.end) return false;
        } else if (section == 5) {
            uint32_t flags, minimum, maximum;
            if (memory || !u32(&r, &count) || count != 1 ||
                !u32(&r, &flags) || flags != 1 || !u32(&r, &minimum) ||
                !u32(&r, &maximum) || minimum != 1 || maximum != 1 ||
                r.at != r.end) return false;
            memory = true;
        } else if (section == 3) {
            if (!u32(&r, &count) || count > 64) return false;
        } else if (section == 7) {
            if (!u32(&r, &count) || count > 16) return false;
            for (uint32_t i = 0; i < count; ++i) {
                reader field;
                uint32_t index;
                if (!name(&r, &field) || r.at == r.end) return false;
                uint8_t kind = *r.at++;
                if (!u32(&r, &index) || equals(field, "__post_instantiate") ||
                    equals(field, "__wasm_call_ctors") || equals(field, "_initialize"))
                    return false;
                if (equals(field, "kaos_configure")) configure = kind == 0;
                if (equals(field, "kaos_main")) main = kind == 0;
            }
            if (r.at != r.end) return false;
        }
    }
    return memory && configure && main;
}

bool core_wamr_profile(const uint8_t *bytes, size_t size)
{
    const core_native_imports empty = {0};
    return core_wamr_profile_with_imports(bytes, size, &empty);
}

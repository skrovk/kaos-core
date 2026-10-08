#include "kaos_predicate.h"

#include <string.h>

bool core_predicate_init(core_predicate *out, const core_predicate_token *tokens,
                         size_t count, size_t channel_count)
{
    if (out == NULL || tokens == NULL || count == 0 ||
        count > CORE_MAX_PREDICATE_TOKENS || channel_count > CORE_MAX_CHANNELS) {
        return false;
    }

    bool values[CORE_MAX_PREDICATE_DEPTH];
    uint8_t depths[CORE_MAX_PREDICATE_DEPTH];
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        const core_predicate_token token = tokens[i];
        switch (token.kind) {
        case CORE_PREDICATE_FALSE:
        case CORE_PREDICATE_TRUE:
        case CORE_PREDICATE_CHANNEL:
            if (used == CORE_MAX_PREDICATE_DEPTH ||
                (token.kind == CORE_PREDICATE_CHANNEL &&
                 token.channel >= channel_count)) {
                return false;
            }
            /* Every declared channel belongs to the admission-time full set. */
            values[used] = token.kind != CORE_PREDICATE_FALSE;
            depths[used] = 1;
            ++used;
            break;
        case CORE_PREDICATE_AND:
        case CORE_PREDICATE_OR: {
            if (used < 2) {
                return false;
            }
            --used;
            const uint8_t depth = depths[used - 1] > depths[used]
                ? depths[used - 1] : depths[used];
            if (depth == CORE_MAX_PREDICATE_DEPTH) {
                return false;
            }
            depths[used - 1] = depth + 1;
            values[used - 1] = token.kind == CORE_PREDICATE_AND
                ? values[used - 1] && values[used]
                : values[used - 1] || values[used];
            break;
        }
        default:
            return false;
        }
    }
    if (used != 1 || !values[0]) {
        return false;
    }

    memmove(out->tokens, tokens, count * sizeof(*tokens));
    out->count = (uint8_t)count;
    return true;
}

bool core_predicate_eval(const core_predicate *predicate, core_channel_set channels)
{
    if (predicate == NULL || predicate->count == 0 ||
        predicate->count > CORE_MAX_PREDICATE_TOKENS) {
        return false;
    }
    bool values[CORE_MAX_PREDICATE_DEPTH];
    size_t used = 0;
    for (size_t i = 0; i < predicate->count; ++i) {
        const core_predicate_token token = predicate->tokens[i];
        switch (token.kind) {
        case CORE_PREDICATE_FALSE:
        case CORE_PREDICATE_TRUE:
        case CORE_PREDICATE_CHANNEL:
            if (used == CORE_MAX_PREDICATE_DEPTH ||
                (token.kind == CORE_PREDICATE_CHANNEL &&
                 token.channel >= CORE_MAX_CHANNELS)) {
                return false;
            }
            values[used++] = token.kind == CORE_PREDICATE_CHANNEL
                ? (channels & (UINT32_C(1) << token.channel)) != 0
                : token.kind == CORE_PREDICATE_TRUE;
            break;
        case CORE_PREDICATE_AND:
        case CORE_PREDICATE_OR:
            if (used < 2) {
                return false;
            }
            --used;
            values[used - 1] = token.kind == CORE_PREDICATE_AND
                ? values[used - 1] && values[used]
                : values[used - 1] || values[used];
            break;
        default:
            return false;
        }
    }
    return used == 1 && values[0];
}

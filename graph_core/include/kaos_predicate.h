#ifndef KAOS_PREDICATE_H
#define KAOS_PREDICATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Provisional shared host/embedded limits, pending device qualification.
 * Depth counts a leaf as one level and each binary operator as another. */
#define CORE_MAX_CHANNELS 16
#define CORE_MAX_PREDICATE_TOKENS 63
#define CORE_MAX_PREDICATE_DEPTH 16

typedef uint16_t core_channel_set;

typedef enum {
    CORE_PREDICATE_FALSE,
    CORE_PREDICATE_TRUE,
    CORE_PREDICATE_CHANNEL,
    CORE_PREDICATE_AND,
    CORE_PREDICATE_OR
} core_predicate_kind;

/* Internal representation, not a wire encoding. Binary AND/OR follow their
 * operands: (A or B) and C is A B OR C AND. channel is a zero-based index
 * into the request's declared channel table; ignored for non-channel tokens. */
typedef struct {
    uint8_t kind; /* core_predicate_kind */
    uint8_t channel;
} core_predicate_token;

/* Caller-owned validated copy. Keep this record and its declared channel
 * ordering immutable throughout the operation. Fields are not a mutation API. */
typedef struct {
    core_predicate_token tokens[CORE_MAX_PREDICATE_TOKENS];
    uint8_t count;
} core_predicate;

/* Validate every token, all three bounds, and P(all declared channels).
 * No allocation, recursion, or graph side effects. Failure leaves out unchanged.
 * tokens must contain count readable elements when count is within bounds.
 * Null pointers are rejected. The input may overlap out's token storage. */
bool core_predicate_init(core_predicate *out, const core_predicate_token *tokens,
                         size_t count, size_t channel_count);

/* Requires a successfully initialized, unmodified predicate. Bit i represents
 * declared channel i; other bits are ignored. The caller supplies complete
 * prepared/active channels or possible channels, never partial endpoint facts.
 * Reuse the same predicate for commit, startup, and feasibility. */
bool core_predicate_eval(const core_predicate *predicate, core_channel_set channels);

#endif

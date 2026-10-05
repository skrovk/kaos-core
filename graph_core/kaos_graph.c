#include "kaos_graph.h"

bool go_next_op_id(uint64_t *last, go_op_id *out)
{
    if (*last == UINT64_MAX) {
        return false;
    }
    out->value = ++*last;
    return true;
}

bool go_duration_us(uint64_t milliseconds, uint64_t *out)
{
    if (milliseconds == 0 || milliseconds > UINT64_MAX / 1000) {
        return false;
    }
    *out = milliseconds * 1000;
    return true;
}

bool go_deadline(uint64_t now_us, uint64_t allowance_us, uint64_t *out)
{
    if (allowance_us == 0 || allowance_us > UINT64_MAX - now_us) {
        return false;
    }
    *out = now_us + allowance_us;
    return true;
}

bool go_before_deadline(uint64_t now_us, uint64_t deadline_us)
{
    return now_us < deadline_us;
}

bool go_lifetime_quiescent(const go_lifetime *owner)
{
    for (size_t i = 0; i < GO_MAX_ACCESSES; ++i) {
        if (owner->accesses[i] != 0) {
            return false;
        }
    }
    return true;
}

bool go_lifetime_charge(go_lifetime *owner, size_t bytes)
{
    if (owner->closing || bytes == 0 ||
        bytes > owner->byte_limit - owner->bytes ||
        owner->allocations == SIZE_MAX) {
        return false;
    }
    owner->bytes += bytes;
    ++owner->allocations;
    if (owner->bytes > owner->peak_bytes) {
        owner->peak_bytes = owner->bytes;
    }
    return true;
}

bool go_lifetime_uncharge(go_lifetime *owner, size_t bytes)
{
    if (!owner->closing || !go_lifetime_quiescent(owner) || bytes == 0 ||
        bytes > owner->bytes || owner->allocations == 0 ||
        (owner->allocations == 1 && bytes != owner->bytes) ||
        (owner->allocations > 1 && owner->bytes - bytes < owner->allocations - 1)) {
        return false;
    }
    owner->bytes -= bytes;
    --owner->allocations;
    return true;
}

bool go_access_acquire(go_lifetime *owner, go_access *out)
{
    if (owner->closing || owner->last_ticket == UINT64_MAX) {
        return false;
    }
    for (size_t i = 0; i < GO_MAX_ACCESSES; ++i) {
        if (owner->accesses[i] == 0) {
            const uint64_t ticket = ++owner->last_ticket;
            owner->accesses[i] = ticket;
            *out = (go_access){ .owner = owner, .ticket = ticket };
            return true;
        }
    }
    return false;
}

bool go_access_release(go_lifetime *owner, go_access access)
{
    if (access.owner != owner || access.ticket == 0) {
        return false;
    }
    for (size_t i = 0; i < GO_MAX_ACCESSES; ++i) {
        if (owner->accesses[i] == access.ticket) {
            owner->accesses[i] = 0;
            return true;
        }
    }
    return false;
}

void go_lifetime_close(go_lifetime *owner)
{
    owner->closing = true;
}

bool go_lifetime_reclaimed(const go_lifetime *owner)
{
    return owner->closing && go_lifetime_quiescent(owner) &&
           owner->bytes == 0 && owner->allocations == 0;
}

bool go_lifetime_reopen(go_lifetime *owner)
{
    if (!go_lifetime_reclaimed(owner) || owner->last_ticket == UINT64_MAX) {
        return false;
    }
    owner->closing = false;
    return true;
}

static bool action_matches(const go_action *action, go_access token)
{
    return action->phase != GO_ACTION_EMPTY && token.ticket != 0 &&
           action->access.owner == token.owner &&
           action->access.ticket == token.ticket;
}

bool go_action_issue(go_action *action, go_lifetime *owner, go_access *out)
{
    if (action->phase != GO_ACTION_EMPTY || !go_access_acquire(owner, out)) {
        return false;
    }
    action->access = *out;
    action->phase = GO_ACTION_ISSUED;
    return true;
}

bool go_action_begin(go_action *action, go_access token)
{
    if (!action_matches(action, token) || action->phase != GO_ACTION_ISSUED) {
        return false;
    }
    if (token.owner->closing) {
        action->result = GO_WORK_CANCELLED;
        action->phase = GO_ACTION_COMPLETED;
        return false;
    }
    action->phase = GO_ACTION_EXECUTING;
    return true;
}

bool go_action_complete(go_action *action, go_access token, go_work_result result)
{
    if (!action_matches(action, token) || action->phase != GO_ACTION_EXECUTING ||
        (result != GO_WORK_SUCCEEDED && result != GO_WORK_FAILED &&
         result != GO_WORK_CANCELLED)) {
        return false;
    }
    action->result = result;
    action->phase = GO_ACTION_COMPLETED;
    return true;
}

bool go_action_accept(go_action *action, go_access token, go_work_result *out)
{
    if (!action_matches(action, token) || action->phase != GO_ACTION_COMPLETED ||
        !go_access_release(token.owner, token)) {
        return false;
    }
    *out = action->result;
    *action = (go_action){0};
    return true;
}

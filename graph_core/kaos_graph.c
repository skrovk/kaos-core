#include "kaos_graph.h"

bool core_duration_us(uint64_t milliseconds, uint64_t *out)
{
    if (milliseconds == 0 || milliseconds > UINT64_MAX / 1000) {
        return false;
    }
    *out = milliseconds * 1000;
    return true;
}

bool core_deadline(uint64_t now_us, uint64_t allowance_us, uint64_t *out)
{
    if (allowance_us == 0 || allowance_us > UINT64_MAX - now_us) {
        return false;
    }
    *out = now_us + allowance_us;
    return true;
}

bool core_before_deadline(uint64_t now_us, uint64_t deadline_us)
{
    return now_us < deadline_us;
}

bool core_lifetime_unclaimed(const core_lifetime *owner)
{
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        if (owner->accesses[i] != 0) {
            return false;
        }
    }
    return true;
}

bool core_lifetime_charge(core_lifetime *owner, size_t bytes)
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

bool core_lifetime_uncharge(core_lifetime *owner, size_t bytes)
{
    if (!owner->closing || !core_lifetime_unclaimed(owner) || bytes == 0 ||
        bytes > owner->bytes || owner->allocations == 0 ||
        (owner->allocations == 1 && bytes != owner->bytes) ||
        (owner->allocations > 1 && owner->bytes - bytes < owner->allocations - 1)) {
        return false;
    }
    owner->bytes -= bytes;
    --owner->allocations;
    return true;
}

bool core_access_acquire(core_lifetime *owner, core_access *out)
{
    if (owner->closing || owner->last_ticket == UINT64_MAX) {
        return false;
    }
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        if (owner->accesses[i] == 0) {
            const uint64_t ticket = ++owner->last_ticket;
            owner->accesses[i] = ticket;
            *out = (core_access){ .owner = owner, .ticket = ticket };
            return true;
        }
    }
    return false;
}

bool core_access_release(core_lifetime *owner, core_access access)
{
    if (access.owner != owner || access.ticket == 0) {
        return false;
    }
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        if (owner->accesses[i] == access.ticket) {
            owner->accesses[i] = 0;
            return true;
        }
    }
    return false;
}

void core_lifetime_close(core_lifetime *owner)
{
    owner->closing = true;
}

bool core_lifetime_reclaimed(const core_lifetime *owner)
{
    return owner->closing && core_lifetime_unclaimed(owner) &&
           owner->bytes == 0 && owner->allocations == 0;
}

bool core_lifetime_reopen(core_lifetime *owner)
{
    if (!core_lifetime_reclaimed(owner) || owner->last_ticket == UINT64_MAX) {
        return false;
    }
    owner->closing = false;
    return true;
}

static bool action_matches(const core_action *action, core_access token)
{
    return action->phase != CORE_ACTION_EMPTY && token.ticket != 0 &&
           action->access.owner == token.owner &&
           action->access.ticket == token.ticket;
}

bool core_action_issue(core_action *action, core_lifetime *owner, core_access *out)
{
    if (action->phase != CORE_ACTION_EMPTY || !core_access_acquire(owner, out)) {
        return false;
    }
    action->access = *out;
    action->phase = CORE_ACTION_ISSUED;
    return true;
}

bool core_action_begin(core_action *action, core_access token)
{
    if (!action_matches(action, token) || action->phase != CORE_ACTION_ISSUED) {
        return false;
    }
    if (token.owner->closing) {
        action->result = CORE_WORK_CANCELLED;
        action->phase = CORE_ACTION_COMPLETED;
        return false;
    }
    action->phase = CORE_ACTION_EXECUTING;
    return true;
}

bool core_action_complete(core_action *action, core_access token, core_work_result result)
{
    if (!action_matches(action, token) || action->phase != CORE_ACTION_EXECUTING ||
        (result != CORE_WORK_SUCCEEDED && result != CORE_WORK_FAILED &&
         result != CORE_WORK_CANCELLED)) {
        return false;
    }
    action->result = result;
    action->phase = CORE_ACTION_COMPLETED;
    return true;
}

bool core_action_accept(core_action *action, core_access token, core_work_result *out)
{
    if (!action_matches(action, token) || action->phase != CORE_ACTION_COMPLETED ||
        !core_access_release(token.owner, token)) {
        return false;
    }
    *out = action->result;
    *action = (core_action){0};
    return true;
}

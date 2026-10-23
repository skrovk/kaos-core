#include "kaos_graph.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* These are new foundation tests, not replacements for catalogue cases.
 * Explicit calls are deterministic barriers: no threads or sleeps collapse
 * issue, execution, owner completion, or control acceptance into one step. */
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void time_boundaries(void)
{
    uint64_t duration = 91;
    CHECK(!core_duration_us(0, &duration) && duration == 91);
    CHECK(!core_duration_us(UINT64_MAX / 1000 + 1, &duration));
    CHECK(duration == 91);
    CHECK(core_duration_us(UINT64_MAX / 1000, &duration));
    CHECK(duration == (UINT64_MAX / 1000) * 1000);
    CHECK(core_duration_us(2, &duration) && duration == 2000);
    uint64_t deadline = 92;
    CHECK(!core_deadline(UINT64_MAX, 1, &deadline) && deadline == 92);
    CHECK(!core_deadline(1, UINT64_MAX, &deadline) && deadline == 92);
    CHECK(!core_deadline(1, 0, &deadline) && deadline == 92);
    CHECK(core_deadline(UINT64_MAX - 1, 1, &deadline));
    CHECK(deadline == UINT64_MAX);
    CHECK(core_deadline(10, duration, &deadline) && deadline == 2010);
    CHECK(core_before_deadline(2009, deadline));
    CHECK(!core_before_deadline(2010, deadline));
    CHECK(!core_before_deadline(2011, deadline));
    /* A report queued at 2009 is late when accepted at 2011. */
    CHECK(deadline == 2010);
    uint64_t first, second;
    CHECK(core_clock_now(&first));
    CHECK(core_clock_now(&second));
    CHECK(second >= first);
}

static void access_bounds_and_reuse(void)
{
    core_lifetime owner = CORE_LIFETIME_INIT(100);
    core_lifetime other = CORE_LIFETIME_INIT(100);
    core_access accesses[CORE_MAX_ACCESSES];
    core_access foreign;
    CHECK(core_access_acquire(&other, &foreign));
    CHECK(!core_lifetime_reclaimed(&owner));
    CHECK(!core_lifetime_reopen(&owner));
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        CHECK(core_access_acquire(&owner, &accesses[i]));
    }
    core_access unchanged = {&other, 81};
    CHECK(!core_access_acquire(&owner, &unchanged));
    CHECK(unchanged.owner == &other && unchanged.ticket == 81);
    CHECK(!core_access_release(&owner, foreign));
    CHECK(!core_access_release(&owner, (core_access){&owner, 0}));
    core_lifetime_close(&owner);
    core_lifetime_close(&owner);
    CHECK(!core_access_acquire(&owner, &unchanged));
    CHECK(!core_lifetime_reopen(&owner));
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        CHECK(!core_lifetime_unclaimed(&owner));
        CHECK(core_access_release(&owner, accesses[i]));
        CHECK(!core_access_release(&owner, accesses[i]));
    }
    CHECK(core_lifetime_reclaimed(&owner));
    CHECK(core_lifetime_reopen(&owner));
    core_access fresh;
    CHECK(core_access_acquire(&owner, &fresh));
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        CHECK(fresh.ticket != accesses[i].ticket);
        CHECK(!core_access_release(&owner, accesses[i]));
    }
    CHECK(!core_lifetime_unclaimed(&owner));
    CHECK(core_access_release(&owner, fresh));
    CHECK(core_access_release(&other, foreign));

    /* A synthetic near-exhaustion initial state avoids 2^64 iterations. */
    core_lifetime exhausted = {.last_ticket = UINT64_MAX - 1};
    CHECK(core_access_acquire(&exhausted, &fresh));
    CHECK(fresh.ticket == UINT64_MAX);
    CHECK(core_access_release(&exhausted, fresh));
    CHECK(!core_access_acquire(&exhausted, &unchanged));
    core_lifetime_close(&exhausted);
    CHECK(core_lifetime_reclaimed(&exhausted));
    CHECK(!core_lifetime_reopen(&exhausted));
}

static void partial_allocations(void)
{
    for (size_t failed_allocation = 0; failed_allocation < 4; ++failed_allocation) {
        core_lifetime owner = CORE_LIFETIME_INIT(30);
        size_t reservations = 0;
        for (size_t i = 0; i < 3; ++i) {
            CHECK(core_lifetime_charge(&owner, 10));
            ++reservations;
            if (i == failed_allocation) {
                break; /* reservation includes the failed allocation */
            }
        }
        CHECK(owner.bytes == 10 * reservations);
        CHECK(!core_lifetime_uncharge(&owner, 10));
        core_access access;
        CHECK(core_access_acquire(&owner, &access));
        core_lifetime_close(&owner);
        CHECK(!core_lifetime_charge(&owner, 1));
        CHECK(!core_lifetime_uncharge(&owner, 10));
        CHECK(!core_lifetime_reclaimed(&owner));
        CHECK(core_access_release(&owner, access));
        CHECK(core_lifetime_unclaimed(&owner));
        CHECK(!core_lifetime_reclaimed(&owner));
        for (size_t i = 0; i < reservations; ++i) {
            CHECK(core_lifetime_uncharge(&owner, 10));
        }
        CHECK(core_lifetime_reclaimed(&owner));
        CHECK(owner.bytes == 0 && owner.allocations == 0);
        CHECK(owner.peak_bytes == 10 * reservations);
        CHECK(!core_lifetime_uncharge(&owner, 10));
    }

    core_lifetime owner = CORE_LIFETIME_INIT(SIZE_MAX);
    CHECK(!core_lifetime_charge(&owner, 0));
    CHECK(core_lifetime_charge(&owner, SIZE_MAX));
    CHECK(!core_lifetime_charge(&owner, 1));
    core_lifetime_close(&owner);
    CHECK(!core_lifetime_uncharge(&owner, SIZE_MAX - 1));
    CHECK(core_lifetime_uncharge(&owner, SIZE_MAX));
    CHECK(core_lifetime_reclaimed(&owner));
}

static void trace(core_op_id op, const char *boundary, uint64_t now,
                  const core_lifetime *owner, const core_action *action)
{
    printf("op=%" PRIu64 " boundary=%s now_us=%" PRIu64
           " phase=%d closing=%d bytes=%zu allocations=%zu unclaimed=%d\n",
           op.value, boundary, now, action->phase, owner->closing, owner->bytes,
           owner->allocations, core_lifetime_unclaimed(owner));
}

static void action_boundaries(void)
{
    const core_op_id op = {1};
    core_lifetime owner = CORE_LIFETIME_INIT(128);
    core_action action = {0};
    core_access token, fresh;
    core_work_result result = CORE_WORK_FAILED;
    CHECK(core_lifetime_charge(&owner, 128));
    CHECK(!core_lifetime_charge(&owner, 1));
    CHECK(core_action_issue(&action, &owner, &token));
    trace(op, "issue", 10, &owner, &action);
    CHECK(!core_action_issue(&action, &owner, &fresh));
    CHECK(!core_action_complete(&action, token, CORE_WORK_SUCCEEDED));
    CHECK(!core_action_accept(&action, token, &result));
    CHECK(core_action_begin(&action, token));
    trace(op, "execute", 20, &owner, &action);
    CHECK(!core_action_begin(&action, token));
    CHECK(!core_action_complete(&action, token, (core_work_result)99));
    CHECK(core_action_complete(&action, token, CORE_WORK_SUCCEEDED));
    trace(op, "complete", 30, &owner, &action);
    CHECK(!core_action_complete(&action, token, CORE_WORK_FAILED));
    CHECK(!core_lifetime_unclaimed(&owner));
    CHECK(core_action_accept(&action, token, &result));
    CHECK(result == CORE_WORK_SUCCEEDED);
    trace(op, "accept", 40, &owner, &action);
    CHECK(!core_action_accept(&action, token, &result));
    CHECK(core_lifetime_unclaimed(&owner));
    CHECK(owner.bytes == 128);
    core_lifetime_close(&owner);
    CHECK(core_lifetime_uncharge(&owner, 128));
    CHECK(core_lifetime_reclaimed(&owner));
    CHECK(core_lifetime_reopen(&owner));
    CHECK(core_action_issue(&action, &owner, &fresh));
    CHECK(!core_action_begin(&action, token));
    CHECK(!core_action_complete(&action, token, CORE_WORK_SUCCEEDED));
    CHECK(!core_action_accept(&action, token, &result));
    CHECK(action.phase == CORE_ACTION_ISSUED);
    CHECK(core_action_begin(&action, fresh));
    CHECK(core_action_complete(&action, fresh, CORE_WORK_FAILED));
    CHECK(core_action_accept(&action, fresh, &result));
    CHECK(result == CORE_WORK_FAILED);
}

static void closure_races(void)
{
    for (size_t close_at = 0; close_at < 4; ++close_at) {
        core_lifetime owner = CORE_LIFETIME_INIT(16);
        core_action action = {0};
        core_access token;
        core_work_result result;
        CHECK(core_lifetime_charge(&owner, 16));
        CHECK(core_action_issue(&action, &owner, &token));
        if (close_at == 0) {
            core_lifetime_close(&owner);
            CHECK(!core_action_begin(&action, token));
            CHECK(action.phase == CORE_ACTION_COMPLETED);
        } else {
            CHECK(core_action_begin(&action, token));
            if (close_at == 1) {
                core_lifetime_close(&owner);
            }
            CHECK(core_action_complete(&action, token, CORE_WORK_SUCCEEDED));
        }
        if (close_at == 2) {
            core_lifetime_close(&owner);
        }
        CHECK(!core_lifetime_reclaimed(&owner));
        CHECK(!core_lifetime_uncharge(&owner, 16));
        CHECK(core_action_accept(&action, token, &result));
        CHECK(result == (close_at == 0 ? CORE_WORK_CANCELLED : CORE_WORK_SUCCEEDED));
        if (close_at == 3) {
            core_lifetime_close(&owner);
        }
        CHECK(!core_lifetime_reclaimed(&owner));
        CHECK(core_lifetime_uncharge(&owner, 16));
        CHECK(core_lifetime_reclaimed(&owner));
    }
}

static void completion_capacity_and_independence(void)
{
    core_lifetime owner = CORE_LIFETIME_INIT(4);
    core_lifetime other = CORE_LIFETIME_INIT(4);
    core_action actions[CORE_MAX_ACCESSES] = {0};
    core_access tokens[CORE_MAX_ACCESSES];
    core_action unrelated = {0};
    core_action excess = {0};
    core_access extra, foreign;
    core_work_result result;
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        CHECK(core_action_issue(&actions[i], &owner, &tokens[i]));
        CHECK(core_action_begin(&actions[i], tokens[i]));
    }
    CHECK(!core_action_issue(&excess, &owner, &extra));
    CHECK(core_action_issue(&unrelated, &other, &foreign));
    CHECK(!core_action_begin(&actions[0], foreign));
    core_lifetime_close(&owner);
    CHECK(core_action_begin(&unrelated, foreign));
    for (size_t i = 0; i < CORE_MAX_ACCESSES; ++i) {
        CHECK(core_action_complete(&actions[i], tokens[i], CORE_WORK_CANCELLED));
    }
    CHECK(!core_lifetime_reclaimed(&owner));
    /* Accept in reverse order, with duplicate delivery at every boundary. */
    for (size_t i = CORE_MAX_ACCESSES; i > 0; --i) {
        CHECK(core_action_accept(&actions[i - 1], tokens[i - 1], &result));
        CHECK(!core_action_accept(&actions[i - 1], tokens[i - 1], &result));
    }
    CHECK(core_lifetime_reclaimed(&owner));
    CHECK(!other.closing && unrelated.phase == CORE_ACTION_EXECUTING);
    CHECK(core_action_complete(&unrelated, foreign, CORE_WORK_SUCCEEDED));
    CHECK(core_action_accept(&unrelated, foreign, &result));
    CHECK(result == CORE_WORK_SUCCEEDED);
}

static void churn(void)
{
    core_lifetime owner = CORE_LIFETIME_INIT(2);
    core_action action = {0};
    core_access old = {0};
    for (size_t i = 0; i < 10000; ++i) {
        core_access current;
        core_work_result result;
        CHECK(core_lifetime_charge(&owner, 1));
        CHECK(core_lifetime_charge(&owner, 1));
        CHECK(!core_lifetime_charge(&owner, 1));
        CHECK(core_action_issue(&action, &owner, &current));
        CHECK(!core_action_complete(&action, old, CORE_WORK_SUCCEEDED));
        CHECK(core_action_begin(&action, current));
        core_lifetime_close(&owner);
        CHECK(core_action_complete(&action, current, CORE_WORK_CANCELLED));
        CHECK(core_action_accept(&action, current, &result));
        CHECK(!core_lifetime_uncharge(&owner, 2));
        CHECK(core_lifetime_uncharge(&owner, 1));
        CHECK(core_lifetime_uncharge(&owner, 1));
        CHECK(core_lifetime_reclaimed(&owner));
        CHECK(core_lifetime_reopen(&owner));
        old = current;
    }
    CHECK(owner.bytes == 0 && owner.allocations == 0 && owner.peak_bytes == 2);
}

int main(void)
{
    time_boundaries();
    access_bounds_and_reuse();
    partial_allocations();
    action_boundaries();
    closure_races();
    completion_capacity_and_independence();
    churn();
    printf("7 foundation groups passed; lifetime=%zu action=%zu access=%zu bytes\n",
           sizeof(core_lifetime), sizeof(core_action), sizeof(core_access));
    return 0;
}

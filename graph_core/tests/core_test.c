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

static void identities_and_time(void)
{
    uint64_t last = 0;
    go_op_id id = {0};
    CHECK(go_next_op_id(&last, &id) && id.value == 1);
    const go_instance_id instance = {id.value};
    CHECK(go_next_op_id(&last, &id) && id.value == 2);
    CHECK(instance.value == 1);
    last = UINT64_MAX - 1;
    CHECK(go_next_op_id(&last, &id) && id.value == UINT64_MAX);
    CHECK(!go_next_op_id(&last, &id));
    CHECK(last == UINT64_MAX && id.value == UINT64_MAX);

    uint64_t duration = 91;
    CHECK(!go_duration_us(0, &duration) && duration == 91);
    CHECK(!go_duration_us(UINT64_MAX / 1000 + 1, &duration));
    CHECK(duration == 91);
    CHECK(go_duration_us(UINT64_MAX / 1000, &duration));
    CHECK(duration == (UINT64_MAX / 1000) * 1000);
    CHECK(go_duration_us(2, &duration) && duration == 2000);
    uint64_t deadline = 92;
    CHECK(!go_deadline(UINT64_MAX, 1, &deadline) && deadline == 92);
    CHECK(!go_deadline(1, UINT64_MAX, &deadline) && deadline == 92);
    CHECK(!go_deadline(1, 0, &deadline) && deadline == 92);
    CHECK(go_deadline(UINT64_MAX - 1, 1, &deadline));
    CHECK(deadline == UINT64_MAX);
    CHECK(go_deadline(10, duration, &deadline) && deadline == 2010);
    CHECK(go_before_deadline(2009, deadline));
    CHECK(!go_before_deadline(2010, deadline));
    CHECK(!go_before_deadline(2011, deadline));
    /* A report queued at 2009 is late when accepted at 2011. */
    CHECK(deadline == 2010);
    uint64_t first, second;
    CHECK(go_clock_now(&first));
    CHECK(go_clock_now(&second));
    CHECK(second >= first);
}

static void access_bounds_and_reuse(void)
{
    go_lifetime owner = GO_LIFETIME_INIT(100);
    go_lifetime other = GO_LIFETIME_INIT(100);
    go_access accesses[GO_MAX_ACCESSES];
    go_access foreign;
    CHECK(go_access_acquire(&other, &foreign));
    CHECK(!go_lifetime_reclaimed(&owner));
    CHECK(!go_lifetime_reopen(&owner));
    for (size_t i = 0; i < GO_MAX_ACCESSES; ++i) {
        CHECK(go_access_acquire(&owner, &accesses[i]));
    }
    go_access unchanged = {&other, 81};
    CHECK(!go_access_acquire(&owner, &unchanged));
    CHECK(unchanged.owner == &other && unchanged.ticket == 81);
    CHECK(!go_access_release(&owner, foreign));
    CHECK(!go_access_release(&owner, (go_access){&owner, 0}));
    go_lifetime_close(&owner);
    go_lifetime_close(&owner);
    CHECK(!go_access_acquire(&owner, &unchanged));
    CHECK(!go_lifetime_reopen(&owner));
    for (size_t i = 0; i < GO_MAX_ACCESSES; ++i) {
        CHECK(!go_lifetime_quiescent(&owner));
        CHECK(go_access_release(&owner, accesses[i]));
        CHECK(!go_access_release(&owner, accesses[i]));
    }
    CHECK(go_lifetime_reclaimed(&owner));
    CHECK(go_lifetime_reopen(&owner));
    go_access fresh;
    CHECK(go_access_acquire(&owner, &fresh));
    for (size_t i = 0; i < GO_MAX_ACCESSES; ++i) {
        CHECK(fresh.ticket != accesses[i].ticket);
        CHECK(!go_access_release(&owner, accesses[i]));
    }
    CHECK(!go_lifetime_quiescent(&owner));
    CHECK(go_access_release(&owner, fresh));
    CHECK(go_access_release(&other, foreign));

    /* A synthetic near-exhaustion initial state avoids 2^64 iterations. */
    go_lifetime exhausted = {.last_ticket = UINT64_MAX - 1};
    CHECK(go_access_acquire(&exhausted, &fresh));
    CHECK(fresh.ticket == UINT64_MAX);
    CHECK(go_access_release(&exhausted, fresh));
    CHECK(!go_access_acquire(&exhausted, &unchanged));
    go_lifetime_close(&exhausted);
    CHECK(go_lifetime_reclaimed(&exhausted));
    CHECK(!go_lifetime_reopen(&exhausted));
}

static void partial_allocations(void)
{
    for (size_t failed_allocation = 0; failed_allocation < 4; ++failed_allocation) {
        go_lifetime owner = GO_LIFETIME_INIT(30);
        size_t reservations = 0;
        for (size_t i = 0; i < 3; ++i) {
            CHECK(go_lifetime_charge(&owner, 10));
            ++reservations;
            if (i == failed_allocation) {
                break; /* reservation includes the failed allocation */
            }
        }
        CHECK(owner.bytes == 10 * reservations);
        CHECK(!go_lifetime_uncharge(&owner, 10));
        go_access access;
        CHECK(go_access_acquire(&owner, &access));
        go_lifetime_close(&owner);
        CHECK(!go_lifetime_charge(&owner, 1));
        CHECK(!go_lifetime_uncharge(&owner, 10));
        CHECK(!go_lifetime_reclaimed(&owner));
        CHECK(go_access_release(&owner, access));
        CHECK(go_lifetime_quiescent(&owner));
        CHECK(!go_lifetime_reclaimed(&owner));
        for (size_t i = 0; i < reservations; ++i) {
            CHECK(go_lifetime_uncharge(&owner, 10));
        }
        CHECK(go_lifetime_reclaimed(&owner));
        CHECK(owner.bytes == 0 && owner.allocations == 0);
        CHECK(owner.peak_bytes == 10 * reservations);
        CHECK(!go_lifetime_uncharge(&owner, 10));
    }

    go_lifetime owner = GO_LIFETIME_INIT(SIZE_MAX);
    CHECK(!go_lifetime_charge(&owner, 0));
    CHECK(go_lifetime_charge(&owner, SIZE_MAX));
    CHECK(!go_lifetime_charge(&owner, 1));
    go_lifetime_close(&owner);
    CHECK(!go_lifetime_uncharge(&owner, SIZE_MAX - 1));
    CHECK(go_lifetime_uncharge(&owner, SIZE_MAX));
    CHECK(go_lifetime_reclaimed(&owner));
}

static void trace(go_op_id op, const char *boundary, uint64_t now,
                  const go_lifetime *owner, const go_action *action)
{
    printf("op=%" PRIu64 " boundary=%s now_us=%" PRIu64
           " phase=%d closing=%d bytes=%zu allocations=%zu quiescent=%d\n",
           op.value, boundary, now, action->phase, owner->closing, owner->bytes,
           owner->allocations, go_lifetime_quiescent(owner));
}

static void action_boundaries(void)
{
    const go_op_id op = {1};
    go_lifetime owner = GO_LIFETIME_INIT(128);
    go_action action = {0};
    go_access token, fresh;
    go_work_result result = GO_WORK_FAILED;
    CHECK(go_lifetime_charge(&owner, 128));
    CHECK(!go_lifetime_charge(&owner, 1));
    CHECK(go_action_issue(&action, &owner, &token));
    trace(op, "issue", 10, &owner, &action);
    CHECK(!go_action_issue(&action, &owner, &fresh));
    CHECK(!go_action_complete(&action, token, GO_WORK_SUCCEEDED));
    CHECK(!go_action_accept(&action, token, &result));
    CHECK(go_action_begin(&action, token));
    trace(op, "execute", 20, &owner, &action);
    CHECK(!go_action_begin(&action, token));
    CHECK(!go_action_complete(&action, token, (go_work_result)99));
    CHECK(go_action_complete(&action, token, GO_WORK_SUCCEEDED));
    trace(op, "complete", 30, &owner, &action);
    CHECK(!go_action_complete(&action, token, GO_WORK_FAILED));
    CHECK(!go_lifetime_quiescent(&owner));
    CHECK(go_action_accept(&action, token, &result));
    CHECK(result == GO_WORK_SUCCEEDED);
    trace(op, "accept", 40, &owner, &action);
    CHECK(!go_action_accept(&action, token, &result));
    CHECK(go_lifetime_quiescent(&owner));
    CHECK(owner.bytes == 128);
    go_lifetime_close(&owner);
    CHECK(go_lifetime_uncharge(&owner, 128));
    CHECK(go_lifetime_reclaimed(&owner));
    CHECK(go_lifetime_reopen(&owner));
    CHECK(go_action_issue(&action, &owner, &fresh));
    CHECK(!go_action_begin(&action, token));
    CHECK(!go_action_complete(&action, token, GO_WORK_SUCCEEDED));
    CHECK(!go_action_accept(&action, token, &result));
    CHECK(action.phase == GO_ACTION_ISSUED);
    CHECK(go_action_begin(&action, fresh));
    CHECK(go_action_complete(&action, fresh, GO_WORK_FAILED));
    CHECK(go_action_accept(&action, fresh, &result));
    CHECK(result == GO_WORK_FAILED);
}

static void closure_races(void)
{
    for (size_t close_at = 0; close_at < 4; ++close_at) {
        go_lifetime owner = GO_LIFETIME_INIT(16);
        go_action action = {0};
        go_access token;
        go_work_result result;
        CHECK(go_lifetime_charge(&owner, 16));
        CHECK(go_action_issue(&action, &owner, &token));
        if (close_at == 0) {
            go_lifetime_close(&owner);
            CHECK(!go_action_begin(&action, token));
            CHECK(action.phase == GO_ACTION_COMPLETED);
        } else {
            CHECK(go_action_begin(&action, token));
            if (close_at == 1) {
                go_lifetime_close(&owner);
            }
            CHECK(go_action_complete(&action, token, GO_WORK_SUCCEEDED));
        }
        if (close_at == 2) {
            go_lifetime_close(&owner);
        }
        CHECK(!go_lifetime_reclaimed(&owner));
        CHECK(!go_lifetime_uncharge(&owner, 16));
        CHECK(go_action_accept(&action, token, &result));
        CHECK(result == (close_at == 0 ? GO_WORK_CANCELLED : GO_WORK_SUCCEEDED));
        if (close_at == 3) {
            go_lifetime_close(&owner);
        }
        CHECK(!go_lifetime_reclaimed(&owner));
        CHECK(go_lifetime_uncharge(&owner, 16));
        CHECK(go_lifetime_reclaimed(&owner));
    }
}

static void completion_capacity_and_independence(void)
{
    go_lifetime owner = GO_LIFETIME_INIT(4);
    go_lifetime other = GO_LIFETIME_INIT(4);
    go_action actions[GO_MAX_ACCESSES] = {0};
    go_access tokens[GO_MAX_ACCESSES];
    go_action unrelated = {0};
    go_action excess = {0};
    go_access extra, foreign;
    go_work_result result;
    for (size_t i = 0; i < GO_MAX_ACCESSES; ++i) {
        CHECK(go_action_issue(&actions[i], &owner, &tokens[i]));
        CHECK(go_action_begin(&actions[i], tokens[i]));
    }
    CHECK(!go_action_issue(&excess, &owner, &extra));
    CHECK(go_action_issue(&unrelated, &other, &foreign));
    CHECK(!go_action_begin(&actions[0], foreign));
    go_lifetime_close(&owner);
    CHECK(go_action_begin(&unrelated, foreign));
    for (size_t i = 0; i < GO_MAX_ACCESSES; ++i) {
        CHECK(go_action_complete(&actions[i], tokens[i], GO_WORK_CANCELLED));
    }
    CHECK(!go_lifetime_reclaimed(&owner));
    /* Accept in reverse order, with duplicate delivery at every boundary. */
    for (size_t i = GO_MAX_ACCESSES; i > 0; --i) {
        CHECK(go_action_accept(&actions[i - 1], tokens[i - 1], &result));
        CHECK(!go_action_accept(&actions[i - 1], tokens[i - 1], &result));
    }
    CHECK(go_lifetime_reclaimed(&owner));
    CHECK(!other.closing && unrelated.phase == GO_ACTION_EXECUTING);
    CHECK(go_action_complete(&unrelated, foreign, GO_WORK_SUCCEEDED));
    CHECK(go_action_accept(&unrelated, foreign, &result));
    CHECK(result == GO_WORK_SUCCEEDED);
}

static void churn(void)
{
    go_lifetime owner = GO_LIFETIME_INIT(2);
    go_action action = {0};
    go_access old = {0};
    for (size_t i = 0; i < 10000; ++i) {
        go_access current;
        go_work_result result;
        CHECK(go_lifetime_charge(&owner, 1));
        CHECK(go_lifetime_charge(&owner, 1));
        CHECK(!go_lifetime_charge(&owner, 1));
        CHECK(go_action_issue(&action, &owner, &current));
        CHECK(!go_action_complete(&action, old, GO_WORK_SUCCEEDED));
        CHECK(go_action_begin(&action, current));
        go_lifetime_close(&owner);
        CHECK(go_action_complete(&action, current, GO_WORK_CANCELLED));
        CHECK(go_action_accept(&action, current, &result));
        CHECK(!go_lifetime_uncharge(&owner, 2));
        CHECK(go_lifetime_uncharge(&owner, 1));
        CHECK(go_lifetime_uncharge(&owner, 1));
        CHECK(go_lifetime_reclaimed(&owner));
        CHECK(go_lifetime_reopen(&owner));
        old = current;
    }
    CHECK(owner.bytes == 0 && owner.allocations == 0 && owner.peak_bytes == 2);
}

int main(void)
{
    identities_and_time();
    access_bounds_and_reuse();
    partial_allocations();
    action_boundaries();
    closure_races();
    completion_capacity_and_independence();
    churn();
    printf("7 foundation groups passed; lifetime=%zu action=%zu access=%zu bytes\n",
           sizeof(go_lifetime), sizeof(go_action), sizeof(go_access));
    return 0;
}

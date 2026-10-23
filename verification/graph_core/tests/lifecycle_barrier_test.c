#define _POSIX_C_SOURCE 200809L
#include "admission_fixture.h"
#include "kaos_artifact.h"
#include "kaos_node.h"
#include "kaos_wamr.h"

#include <pthread.h>
#include <time.h>

/* Verification-only entry gates. Never wait while holding registry/mailbox.
 * The ledger describes this imposed partial order, not every worker event. */
enum event {
    ARMED, REACHED, UNRELATED_STARTED, DECISION_APPLIED, RELEASED,
    ENTRY_CHECKED, TARGET_FINISHED, UNRELATED_FINISHED
};
enum fault { NO_FAULT, REMOVE, EXPIRE };
typedef struct {
    enum event event;
    unsigned value;
} observation;
typedef struct {
    observation events[16];
    size_t count;
} ledger;
typedef struct {
    core_admission pool;
    core_node_service nodes;
    pthread_mutex_t registry, gate;
    pthread_cond_t changed;
    uint64_t now;
    size_t target;
    unsigned boundary, calls[CORE_MAX_NODE_RESERVATIONS];
    unsigned configured[CORE_MAX_NODE_RESERVATIONS], entered[CORE_MAX_NODE_RESERVATIONS];
    unsigned denied[CORE_MAX_NODE_RESERVATIONS];
    bool held, reached;
    ledger trace;
} fixture;

static _Thread_local size_t guard_slot;
static _Thread_local bool selected_entry, entry_allowed;
static uint8_t *artifact;
static size_t artifact_size;

static uint64_t monotonic_us(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static void append_locked(fixture *f, enum event event, unsigned value)
{
    CHECK(f->trace.count < sizeof(f->trace.events) / sizeof(f->trace.events[0]));
    f->trace.events[f->trace.count++] = (observation){event, value};
}

static void record(fixture *f, enum event event, unsigned value)
{
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    append_locked(f, event, value);
    CHECK(pthread_mutex_unlock(&f->gate) == 0);
}

static core_runtime_state snapshot(fixture *f, size_t slot)
{
    core_node_runtime *r = &f->nodes.runtime;
    const core_runtime_state state = *r->lock(r->context, slot);
    r->unlock(r->context, slot);
    return state;
}

static bool entry_lock(void *context, size_t slot)
{
    fixture *f = context;
    guard_slot = slot;
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    selected_entry = slot == f->target && ++f->calls[slot] == f->boundary;
    if (selected_entry) {
        CHECK(f->held && !f->reached);
        f->reached = true;
        append_locked(f, REACHED, f->boundary);
        CHECK(pthread_cond_broadcast(&f->changed) == 0);
        struct timespec limit;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &limit) == 0);
        limit.tv_sec += 10;
        while (f->held)
            CHECK(pthread_cond_timedwait(&f->changed, &f->gate, &limit) == 0);
    }
    CHECK(pthread_mutex_unlock(&f->gate) == 0);
    CHECK(pthread_mutex_lock(&f->registry) == 0);
    entry_allowed = core_node_start_allowed(&f->nodes, slot, f->now);
    return entry_allowed;
}

static void entry_unlock(void *context)
{
    fixture *f = context;
    /* WAMR has made its final entry decision while retaining the registry.
     * Sampling its mailbox here positively observes execution publication. */
    const core_runtime_state state = snapshot(f, guard_slot);
    if (entry_allowed) {
        CHECK(state.configuration_active != state.main_active);
        f->configured[guard_slot] += state.configuration_active;
        f->entered[guard_slot] += state.main_active;
    } else {
        CHECK(!state.configuration_active && !state.main_active);
        ++f->denied[guard_slot];
    }
    if (selected_entry) record(f, ENTRY_CHECKED, entry_allowed);
    CHECK(pthread_mutex_unlock(&f->registry) == 0);
}

static void step(fixture *f)
{
    CHECK(pthread_mutex_lock(&f->registry) == 0);
    core_node_step(&f->nodes, f->now);
    CHECK(pthread_mutex_unlock(&f->registry) == 0);
}

static void pause_once(uint64_t deadline)
{
    CHECK(monotonic_us() < deadline);
    const struct timespec pause = {.tv_nsec = 1000000};
    nanosleep(&pause, NULL);
}

static const core_admission_record *submit(fixture *f, const fixture_request *request)
{
    uint8_t wire[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(request, wire);
    const core_admission_record *result;
    CHECK(pthread_mutex_lock(&f->registry) == 0);
    CHECK(core_request_submit(&f->pool, 99, wire, size, f->now, &result) ==
          CORE_ADMIT_ACCEPTED);
    /* Admitted removal must affect entry permission before registry release. */
    core_node_step(&f->nodes, f->now);
    CHECK(pthread_mutex_unlock(&f->registry) == 0);
    return result;
}

static const core_admission_record *add(fixture *f, uint64_t id)
{
    fixture_request request = fixture_node(id, id * 10);
    request.node.artifact_bytes = (uint32_t)artifact_size;
    request.node.artifact_crc32 = core_artifact_crc32(artifact, artifact_size);
    request.node.required_bytes = 131072;
    request.node.addition_allowance_us = id == 1 ? 30000000 : 60000000;
    request.node.cleanup_allowance_us = 5000000;
    const core_admission_record *r = submit(f, &request);
    uint8_t *copy = malloc(artifact_size);
    CHECK(copy != NULL);
    memcpy(copy, artifact, artifact_size);
    CHECK(pthread_mutex_lock(&f->registry) == 0);
    CHECK(core_lifetime_charge(&r->node->lifetime, artifact_size));
    CHECK(core_node_artifact(&f->nodes, r->id, copy, artifact_size, f->now));
    CHECK(pthread_mutex_unlock(&f->registry) == 0);
    return r;
}

static const core_admission_record *remove_node(fixture *f, uint64_t operation,
                                               uint64_t instance)
{
    const fixture_request request = {.id = {operation}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = instance * 10,
        .instance = {instance}, .allowance_us = 5000000}};
    return submit(f, &request);
}

static void await_success(fixture *f, const core_admission_record *r)
{
    const uint64_t limit = monotonic_us() + 5000000;
    while (core_report_outcome(&r->evidence) != CORE_OUTCOME_SUCCEEDED) {
        CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_PENDING);
        step(f);
        pause_once(limit);
    }
}

static void await_clean(fixture *f, const core_admission_record *r)
{
    const uint64_t limit = monotonic_us() + 5000000;
    while (!(r->evidence.objects[0] & CORE_OBJECT_CLEANED)) {
        step(f);
        pause_once(limit);
    }
    CHECK((r->evidence.objects[0] & (CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED |
          CORE_OBJECT_CLEANED)) == (CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED |
          CORE_OBJECT_CLEANED));
}

static ledger run(unsigned boundary, enum fault fault)
{
    fixture *f = calloc(1, sizeof(*f));
    CHECK(f != NULL);
    CHECK(pthread_mutex_init(&f->registry, NULL) == 0);
    CHECK(pthread_mutex_init(&f->gate, NULL) == 0);
    pthread_condattr_t attributes;
    CHECK(pthread_condattr_init(&attributes) == 0);
    CHECK(pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0);
    CHECK(pthread_cond_init(&f->changed, &attributes) == 0);
    CHECK(pthread_condattr_destroy(&attributes) == 0);
    f->now = monotonic_us();
    f->boundary = boundary;
    f->held = true;
    CHECK(core_admission_init(&f->pool, 131072, 2 * 131072));
    const uint64_t peers[] = {1};
    CHECK(core_admission_configure(&f->pool, 1, 99, peers, 1));
    f->nodes.admission = &f->pool;
    CHECK(core_wamr_init(&f->nodes.runtime, NULL, NULL));
    CHECK(core_wamr_attach_entry_guard(f, entry_lock, entry_unlock));
    record(f, ARMED, boundary);
    const core_admission_record *target = add(f, 1);
    CHECK(target->node == &f->pool.nodes[f->target]);
    core_reserved_node *reservation = target->node;
    const uint64_t limit = monotonic_us() + 5000000;
    for (;;) {
        step(f);
        CHECK(pthread_mutex_lock(&f->gate) == 0);
        const bool reached = f->reached;
        CHECK(pthread_mutex_unlock(&f->gate) == 0);
        if (reached) break;
        pause_once(limit);
    }
    CHECK(snapshot(f, f->target).prepared);
    CHECK(!(target->evidence.history & CORE_HISTORY_STARTED));

    /* A second actual worker reaches accepted INIT while the first is held.
     * This cannot succeed if the barrier retains registry or mailbox locks. */
    const core_admission_record *other = add(f, 2);
    const size_t other_slot = (size_t)(other->node - f->pool.nodes);
    core_reserved_node *other_node = other->node;
    await_success(f, other);
    CHECK(snapshot(f, other_slot).main_active);
    CHECK(other_node->lifetime.bytes == 131072);
    record(f, UNRELATED_STARTED, 2);

    const core_admission_record *removal = NULL;
    if (fault == REMOVE) removal = remove_node(f, 3, 1);
    if (fault == EXPIRE) {
        CHECK(pthread_mutex_lock(&f->registry) == 0);
        f->now = target->deadline_us; /* strict equality; no warmup timing */
        core_node_step(&f->nodes, f->now);
        CHECK(pthread_mutex_unlock(&f->registry) == 0);
        CHECK(target->evidence.objects[0] & CORE_OBJECT_EXPIRED);
    }
    const core_runtime_state held = snapshot(f, f->target);
    CHECK(!held.configuration_active && !held.main_active && !held.init_pending);
    CHECK(!held.disposed && reservation->occupied);
    CHECK(reservation->lifetime.bytes == 131072 && f->pool.reserved_bytes == 2 * 131072);
    CHECK(!(target->evidence.objects[0] & CORE_OBJECT_CLEANED));
    CHECK(held.cancel_requested == (fault != NO_FAULT));
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    CHECK(f->reached && f->held);
    append_locked(f, DECISION_APPLIED, fault);
    append_locked(f, RELEASED, boundary);
    f->held = false;
    CHECK(pthread_cond_broadcast(&f->changed) == 0);
    CHECK(pthread_mutex_unlock(&f->gate) == 0);

    if (fault == NO_FAULT) {
        await_success(f, target); /* positive witness for both gates */
        removal = remove_node(f, 3, 1);
    }
    await_clean(f, target);
    CHECK(snapshot(f, f->target).disposed);
    CHECK(!reservation->occupied && core_lifetime_reclaimed(&reservation->lifetime));
    CHECK(target->node == NULL && f->pool.reserved_bytes == 131072);
    if (removal) CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_report_outcome(&target->evidence) ==
          (fault == NO_FAULT ? CORE_OUTCOME_SUCCEEDED : CORE_OUTCOME_FAILED_CLEAN));
    CHECK(pthread_mutex_lock(&f->registry) == 0);
    CHECK(f->configured[f->target] == (fault == NO_FAULT || boundary == 2));
    CHECK(f->entered[f->target] == (fault == NO_FAULT));
    CHECK(f->denied[f->target] == (fault != NO_FAULT));
    CHECK(other->node == other_node && other_node->instance.value == 2);
    CHECK(other_node->lifetime.bytes == 131072 && !other_node->lifetime.closing);
    CHECK(f->configured[other_slot] == 1 && f->entered[other_slot] == 1);
    CHECK(pthread_mutex_unlock(&f->registry) == 0);
    CHECK(snapshot(f, other_slot).main_active);
    record(f, TARGET_FINISHED, (unsigned)core_report_outcome(&target->evidence));
    const core_admission_record *other_removal = remove_node(f, 4, 2);
    await_clean(f, other);
    CHECK(core_report_outcome(&other_removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(f->pool.reserved_bytes == 0);
    record(f, UNRELATED_FINISHED, 2);
    core_wamr_shutdown();
    const ledger result = f->trace;
    CHECK(pthread_cond_destroy(&f->changed) == 0);
    CHECK(pthread_mutex_destroy(&f->gate) == 0);
    CHECK(pthread_mutex_destroy(&f->registry) == 0);
    free(f);
    return result;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    const long size = ftell(file);
    CHECK(size > 0 && size <= CORE_MAX_ARTIFACT_BYTES && fseek(file, 0, SEEK_SET) == 0);
    artifact_size = (size_t)size;
    artifact = malloc(artifact_size);
    CHECK(artifact != NULL && fread(artifact, 1, artifact_size, file) == artifact_size);
    CHECK(fclose(file) == 0);
    static const char *names[] = {"armed", "reached", "unrelated_started",
        "decision_applied", "released", "entry_checked", "target_finished", "unrelated_finished"};
    static const char *faults[] = {"none", "removal", "deadline"};
    for (unsigned boundary = 1; boundary <= 2; ++boundary) {
        for (enum fault fault = NO_FAULT; fault <= EXPIRE; ++fault) {
            const ledger first = run(boundary, fault), second = run(boundary, fault);
            CHECK(first.count == 8 && first.count == second.count);
            for (size_t i = 0; i < first.count; ++i) {
                CHECK(first.events[i].event == (enum event)i);
                CHECK(first.events[i].event == second.events[i].event);
                CHECK(first.events[i].value == second.events[i].value);
                printf("{\"boundary\":\"before_%s\",\"fault\":\"%s\",\"sequence\":%zu,"
                       "\"event\":\"%s\",\"value\":%u,\"repeated_order_equal\":true}\n",
                    boundary == 1 ? "configuration" : "main", faults[fault], i,
                    names[first.events[i].event], first.events[i].value);
            }
        }
    }
    free(artifact);
    return 0;
}

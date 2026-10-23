#define _POSIX_C_SOURCE 200809L
#include "admission_fixture.h"
#include "kaos_artifact.h"
#include "kaos_node.h"
#include "kaos_wamr.h"
#include "wasm_export.h"

#include <pthread.h>
#include <time.h>

/* Host-only wrappers call the real engine. The first submitted worker is the
 * target; no other artifact is handed off until its instantiation gate is
 * reached. Gates never retain a registry or runtime mailbox lock. */
enum fault { NO_FAULT, REMOVE, EXPIRE };
enum boundary { PREPARATION, DESTRUCTION, BOUNDARIES };
enum event {
    PREPARATION_HELD, OTHER_STARTED, DECISION_APPLIED, PREPARATION_RELEASED,
    TARGET_STARTED, DESTRUCTION_HELD, CUTOFF_CHECKED, OTHER_CLEANED,
    DESTRUCTION_RELEASED, TARGET_CLEANED
};
typedef struct {
    enum event events[16];
    size_t count;
} ledger;
typedef struct {
    core_admission pool;
    core_node_service nodes;
    pthread_mutex_t registry, gate;
    pthread_cond_t changed;
    uint64_t now;
    size_t target_slot;
    unsigned entries[CORE_MAX_NODE_RESERVATIONS];
    bool reached[BOUNDARIES], released[BOUNDARIES], deinstantiated, unloaded;
    wasm_module_t module;
    wasm_module_inst_t instance;
    ledger trace;
} fixture;

static fixture *active; /* Set before worker creation; cleared after join. */
static uint8_t *artifact;
static size_t artifact_size;
static core_runtime_state snapshot(fixture *f, size_t slot);

static uint64_t monotonic_us(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static void append(fixture *f, enum event event)
{
    CHECK(f->trace.count < sizeof(f->trace.events) / sizeof(f->trace.events[0]));
    f->trace.events[f->trace.count++] = event;
}

static void record(fixture *f, enum event event)
{
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    append(f, event);
    CHECK(pthread_mutex_unlock(&f->gate) == 0);
}

static void wait_flag(fixture *f, const bool *flag)
{
    struct timespec limit;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &limit) == 0);
    limit.tv_sec += 10;
    while (!*flag) CHECK(pthread_cond_timedwait(&f->changed, &f->gate, &limit) == 0);
}

static void hold(fixture *f, enum boundary boundary)
{
    CHECK(!f->reached[boundary] && !f->released[boundary]);
    f->reached[boundary] = true;
    append(f, boundary == PREPARATION ? PREPARATION_HELD : DESTRUCTION_HELD);
    CHECK(pthread_cond_broadcast(&f->changed) == 0);
    wait_flag(f, &f->released[boundary]);
}

wasm_module_inst_t __real_wasm_runtime_instantiate(wasm_module_t, uint32_t,
                                                 uint32_t, char *, uint32_t);
void __real_wasm_runtime_deinstantiate(wasm_module_inst_t);
void __real_wasm_runtime_unload(wasm_module_t);

wasm_module_inst_t __wrap_wasm_runtime_instantiate(wasm_module_t module,
    uint32_t stack, uint32_t heap, char *error, uint32_t error_size)
{
    wasm_module_inst_t instance = __real_wasm_runtime_instantiate(module, stack,
                                                               heap, error, error_size);
    CHECK(instance != NULL);
    fixture *f = active;
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    if (!f->module) {
        f->module = module;
        f->instance = instance;
        /* Actual module/instance allocation finished, but KaOS has not yet
         * assigned the instance or published preparation completion. */
        hold(f, PREPARATION);
    }
    CHECK(pthread_mutex_unlock(&f->gate) == 0);
    return instance;
}

void __wrap_wasm_runtime_deinstantiate(wasm_module_inst_t instance)
{
    fixture *f = active;
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    const bool target = instance == f->instance;
    if (target) hold(f, DESTRUCTION);
    CHECK(pthread_mutex_unlock(&f->gate) == 0);
    /* The execution environment was already destroyed by KaOS; the instance,
     * module, arena and artifact are still owned at the gate above. */
    __real_wasm_runtime_deinstantiate(instance);
    if (target) {
        CHECK(pthread_mutex_lock(&f->gate) == 0);
        CHECK(!f->deinstantiated);
        f->deinstantiated = true;
        CHECK(pthread_mutex_unlock(&f->gate) == 0);
    }
}

void __wrap_wasm_runtime_unload(wasm_module_t module)
{
    fixture *f = active;
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    const bool target = module == f->module;
    CHECK(pthread_mutex_unlock(&f->gate) == 0);
    if (target) CHECK(!snapshot(f, f->target_slot).disposed);
    __real_wasm_runtime_unload(module);
    if (target) {
        CHECK(!snapshot(f, f->target_slot).disposed);
        CHECK(pthread_mutex_lock(&f->gate) == 0);
        CHECK(f->deinstantiated && !f->unloaded);
        f->unloaded = true;
        CHECK(pthread_mutex_unlock(&f->gate) == 0);
    }
}

static core_runtime_state snapshot(fixture *f, size_t slot)
{
    core_node_runtime *runtime = &f->nodes.runtime;
    const core_runtime_state state = *runtime->lock(runtime->context, slot);
    runtime->unlock(runtime->context, slot);
    return state;
}

static bool entry_lock(void *context, size_t slot)
{
    fixture *f = context;
    CHECK(pthread_mutex_lock(&f->registry) == 0);
    ++f->entries[slot];
    return core_node_start_allowed(&f->nodes, slot, f->now);
}

static void entry_unlock(void *context)
{
    CHECK(pthread_mutex_unlock(&((fixture *)context)->registry) == 0);
}

static void step(fixture *f)
{
    CHECK(pthread_mutex_lock(&f->registry) == 0);
    core_node_step(&f->nodes, f->now);
    CHECK(pthread_mutex_unlock(&f->registry) == 0);
}

static const core_admission_record *submit(fixture *f, const fixture_request *request)
{
    uint8_t wire[CORE_MAX_REQUEST_BYTES];
    const size_t size = fixture_encode(request, wire);
    const core_admission_record *result;
    CHECK(pthread_mutex_lock(&f->registry) == 0);
    CHECK(core_request_submit(&f->pool, 99, wire, size, f->now, &result) == CORE_ADMIT_ACCEPTED);
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

static const core_admission_record *remove_node(fixture *f, uint64_t id, uint64_t instance)
{
    const fixture_request request = {.id = {id}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = instance * 10,
        .instance = {instance}, .allowance_us = 5000000}};
    return submit(f, &request);
}

static void await_result(fixture *f, const core_admission_record *r, bool cleaned)
{
    const uint64_t limit = monotonic_us() + 5000000;
    for (;;) {
        step(f);
        if (cleaned ? (r->evidence.objects[0] & CORE_OBJECT_CLEANED) != 0 :
            core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED) return;
        CHECK(monotonic_us() < limit);
        if (!cleaned) CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_PENDING);
        const struct timespec pause = {.tv_nsec = 1000000};
        CHECK(nanosleep(&pause, NULL) == 0);
    }
}

static void await_boundary(fixture *f, enum boundary boundary)
{
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    wait_flag(f, &f->reached[boundary]);
    CHECK(!f->released[boundary]);
    CHECK(pthread_mutex_unlock(&f->gate) == 0);
}

static void release(fixture *f, enum boundary boundary)
{
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    CHECK(f->reached[boundary] && !f->released[boundary]);
    append(f, boundary == PREPARATION ? PREPARATION_RELEASED : DESTRUCTION_RELEASED);
    f->released[boundary] = true;
    CHECK(pthread_cond_broadcast(&f->changed) == 0);
    CHECK(pthread_mutex_unlock(&f->gate) == 0);
}

static void retained(fixture *f, const core_admission_record *r, size_t reservations)
{
    CHECK(r->node && r->node->occupied && r->node->lifetime.bytes == 131072);
    CHECK(r->node->lifetime.allocations == 2 && !core_lifetime_unclaimed(&r->node->lifetime));
    const core_node_execution *e = &f->nodes.executions[r->node - f->pool.nodes];
    CHECK(e->occupied && !e->worker_accepted);
    CHECK(r->node->prepare.phase == CORE_ACTION_EXECUTING);
    CHECK(e->worker.ticket == r->node->prepare.access.ticket);
    CHECK(!(r->evidence.objects[0] & CORE_OBJECT_CLEANED));
    CHECK(f->pool.reserved_bytes == reservations * 131072);
}

static ledger run(enum fault fault, bool cutoff)
{
    fixture *f = calloc(1, sizeof(*f));
    CHECK(f != NULL && active == NULL);
    CHECK(pthread_mutex_init(&f->registry, NULL) == 0);
    CHECK(pthread_mutex_init(&f->gate, NULL) == 0);
    pthread_condattr_t attributes;
    CHECK(pthread_condattr_init(&attributes) == 0);
    CHECK(pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0);
    CHECK(pthread_cond_init(&f->changed, &attributes) == 0);
    CHECK(pthread_condattr_destroy(&attributes) == 0);
    f->now = monotonic_us();
    CHECK(core_admission_init(&f->pool, 131072, 2 * 131072));
    const uint64_t peers[] = {1};
    CHECK(core_admission_configure(&f->pool, 1, 99, peers, 1));
    f->nodes.admission = &f->pool;
    active = f;
    CHECK(core_wamr_init(&f->nodes.runtime, NULL, NULL));
    CHECK(core_wamr_attach_entry_guard(f, entry_lock, entry_unlock));
    const core_admission_record *target = add(f, 1);
    core_reserved_node *reservation = target->node;
    const size_t slot = (size_t)(reservation - f->pool.nodes);
    f->target_slot = slot; /* Published to the worker by preparation release. */
    await_boundary(f, PREPARATION);
    CHECK(!snapshot(f, slot).prepared && !snapshot(f, slot).disposed);
    retained(f, target, 1);

    /* Positive unrelated startup while real preparation is held. */
    const core_admission_record *other = add(f, 2);
    core_reserved_node *other_reservation = other->node;
    const size_t other_slot = (size_t)(other->node - f->pool.nodes);
    await_result(f, other, false);
    CHECK(snapshot(f, other_slot).main_active);
    record(f, OTHER_STARTED);
    const core_admission_record *removal = NULL;
    if (fault == REMOVE) removal = remove_node(f, 3, 1);
    if (fault == EXPIRE) {
        CHECK(pthread_mutex_lock(&f->registry) == 0);
        f->now = target->deadline_us;
        core_node_step(&f->nodes, f->now);
        CHECK(pthread_mutex_unlock(&f->registry) == 0);
        CHECK(target->evidence.objects[0] & CORE_OBJECT_EXPIRED);
    }
    const core_runtime_state preparing = snapshot(f, slot);
    CHECK(!preparing.prepared && !preparing.main_active && !preparing.configuration_active);
    CHECK(preparing.cancel_requested == (fault != NO_FAULT));
    retained(f, target, 2);
    record(f, DECISION_APPLIED);
    release(f, PREPARATION);
    if (fault == NO_FAULT) {
        await_result(f, target, false);
        record(f, TARGET_STARTED);
        removal = remove_node(f, 3, 1);
    }
    await_boundary(f, DESTRUCTION);
    const core_runtime_state destroying = snapshot(f, slot);
    CHECK(destroying.prepared && destroying.cancel_requested && destroying.dispose_requested);
    CHECK(!destroying.main_active && !destroying.configuration_active && !destroying.disposed);
    retained(f, target, 2);
    const core_admission_record *closing = removal ? removal : target;
    const uint64_t deadline = closing->cleanup_deadline_us;
    CHECK(deadline > f->now);
    if (cutoff) {
        CHECK(pthread_mutex_lock(&f->registry) == 0);
        f->now = deadline - 1;
        core_node_step(&f->nodes, f->now);
        CHECK(core_report_outcome(&closing->evidence) == CORE_OUTCOME_PENDING);
        f->now = deadline;
        core_node_step(&f->nodes, f->now);
        CHECK(pthread_mutex_unlock(&f->registry) == 0);
        CHECK(core_report_outcome(&closing->evidence) == CORE_OUTCOME_UNRESOLVED);
        CHECK(closing->evidence.history & CORE_HISTORY_CUTOFF);
    }
    CHECK(closing->cleanup_deadline_us == deadline);
    retained(f, target, 2);
    record(f, CUTOFF_CHECKED);

    /* Fresh unrelated disposal and reclamation, not just a liveness flag,
     * while the target's destruction is blocked. */
    CHECK(other->node == other_reservation && other_reservation->occupied);
    CHECK(other_reservation->established && !other_reservation->lifetime.closing);
    CHECK(other_reservation->lifetime.bytes == 131072);
    const core_runtime_state continuing = snapshot(f, other_slot);
    CHECK(continuing.main_active && !continuing.cancel_requested && !continuing.disposed);
    const core_admission_record *other_removal = remove_node(f, 4, 2);
    await_result(f, other, true);
    CHECK(snapshot(f, other_slot).disposed);
    CHECK(core_report_outcome(&other_removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    retained(f, target, 1);
    CHECK(!snapshot(f, slot).disposed);
    record(f, OTHER_CLEANED);
    release(f, DESTRUCTION);
    await_result(f, target, true);
    CHECK(snapshot(f, slot).disposed);
    CHECK(pthread_mutex_lock(&f->gate) == 0);
    CHECK(f->deinstantiated && f->unloaded);
    CHECK(pthread_mutex_unlock(&f->gate) == 0);
    CHECK((target->evidence.objects[0] & (CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED |
        CORE_OBJECT_CLEANED)) == (CORE_OBJECT_CLEANUP | CORE_OBJECT_STOPPED | CORE_OBJECT_CLEANED));
    CHECK(!reservation->occupied && core_lifetime_reclaimed(&reservation->lifetime));
    CHECK(target->node == NULL && f->pool.reserved_bytes == 0);
    CHECK(core_report_outcome(&target->evidence) ==
          (fault == NO_FAULT ? CORE_OUTCOME_SUCCEEDED : CORE_OUTCOME_FAILED_CLEAN));
    if (removal) CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(pthread_mutex_lock(&f->registry) == 0);
    CHECK(f->entries[slot] == (fault == NO_FAULT ? 2u : 0u));
    CHECK(f->entries[other_slot] == 2);
    CHECK(pthread_mutex_unlock(&f->registry) == 0);
    if (fault != NO_FAULT)
        CHECK(!(target->evidence.history & (CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH |
                                          CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS)));
    step(f);
    CHECK(f->pool.reserved_bytes == 0 && snapshot(f, slot).disposed);
    record(f, TARGET_CLEANED);
    core_wamr_shutdown();
    CHECK(f->deinstantiated && f->unloaded);
    const ledger result = f->trace;
    active = NULL;
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
    CHECK(artifact && fread(artifact, 1, artifact_size, file) == artifact_size);
    CHECK(fclose(file) == 0);
    static const char *names[] = {"preparation_held", "other_started", "decision_applied",
        "preparation_released", "target_started", "destruction_held", "cutoff_checked",
        "other_cleaned", "destruction_released", "target_cleaned"};
    static const char *faults[] = {"none", "removal", "deadline"};
    for (enum fault fault = NO_FAULT; fault <= EXPIRE; ++fault) {
        for (unsigned cutoff = 0; cutoff <= 1; ++cutoff) {
            const ledger first = run(fault, cutoff), second = run(fault, cutoff);
            CHECK(first.count == (fault == NO_FAULT ? 10u : 9u) && first.count == second.count);
            for (size_t i = 0; i < first.count; ++i) {
                const enum event expected = (enum event)(i +
                    (fault != NO_FAULT && i >= TARGET_STARTED ? 1 : 0));
                CHECK(first.events[i] == expected);
                CHECK(first.events[i] == second.events[i]);
                printf("{\"fault\":\"%s\",\"cutoff\":%s,\"sequence\":%zu,"
                       "\"event\":\"%s\",\"repeated_order_equal\":true}\n",
                       faults[fault], cutoff ? "true" : "false", i, names[first.events[i]]);
            }
        }
    }
    free(artifact);
    return 0;
}

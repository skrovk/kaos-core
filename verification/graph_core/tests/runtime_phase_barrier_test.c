#include "admission_fixture.h"
#include "kaos_node.h"

/* A manually scheduled adapter, not a WAMR/thread timing test. prepare queues
 * ownership; explicit releases below perform the delayed work and publication. */
typedef struct {
    core_runtime_state state;
    uint8_t *artifact, *arena;
    bool locked;
} slot;
typedef struct {
    core_admission pool;
    core_node_service nodes;
    slot slots[CORE_MAX_NODE_RESERVATIONS];
    uint64_t now;
} fixture;

enum preparation_fault { NONE, REMOVAL, EXPIRY };

static bool prepare(void *context, size_t index, uint8_t *artifact, size_t bytes,
                    const uint8_t *configuration, size_t configuration_bytes,
                    size_t arena_bytes)
{
    fixture *f = context;
    slot *s = &f->slots[index];
    (void)configuration;
    CHECK(bytes == 16 && arena_bytes == 112 && configuration_bytes == 0);
    CHECK(!s->artifact && !s->arena && !s->locked);
    *s = (slot){.artifact = artifact};
    return true;
}

static core_runtime_state *lock(void *context, size_t index)
{
    slot *s = &((fixture *)context)->slots[index];
    CHECK(!s->locked);
    s->locked = true;
    return &s->state;
}

static void unlock(void *context, size_t index)
{
    slot *s = &((fixture *)context)->slots[index];
    CHECK(s->locked);
    s->locked = false;
}

static void step(fixture *f)
{
    core_node_step(&f->nodes, f->now);
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) CHECK(!f->slots[i].locked);
}

static const core_admission_record *submit(fixture *f, fixture_request request)
{
    uint8_t wire[CORE_MAX_REQUEST_BYTES];
    const core_admission_record *r;
    const size_t bytes = fixture_encode(&request, wire);
    CHECK(core_request_submit(&f->pool, 99, wire, bytes, f->now, &r) == CORE_ADMIT_ACCEPTED);
    step(f);
    return r;
}

static const core_admission_record *add(fixture *f, uint64_t id)
{
    const core_admission_record *r = submit(f, fixture_node(id, id * 10));
    uint8_t *artifact = calloc(16, 1);
    CHECK(artifact && core_lifetime_charge(&r->node->lifetime, 16));
    CHECK(core_node_artifact(&f->nodes, r->id, artifact, 16, f->now));
    return r;
}

static const core_admission_record *remove_node(fixture *f, uint64_t id, uint64_t instance)
{
    return submit(f, (fixture_request){.id = {id}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = instance * 10,
        .instance = {instance}, .allowance_us = 50}});
}

static void complete_preparation(fixture *f, size_t index)
{
    slot *s = &f->slots[index];
    CHECK(!s->locked && s->artifact && !s->arena && !s->state.prepared);
    /* Even work already queued at cancellation can finish private allocation.
     * It must remain owned until disposal, never revive startup permission. */
    s->arena = calloc(112, 1);
    CHECK(s->arena != NULL);
    s->state.prepared = true;
    step(f);
}

static void start(fixture *f, size_t index, const core_admission_record *r)
{
    slot *s = &f->slots[index];
    CHECK(s->state.start_requested && !s->state.cancel_requested);
    CHECK(core_node_start_allowed(&f->nodes, index, f->now));
    s->state.main_active = true;
    s->state.init_pending = true;
    step(f);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(!s->state.init_pending && r->node->established);
}

static void dispose(fixture *f, size_t index)
{
    slot *s = &f->slots[index];
    CHECK(!s->locked && s->state.cancel_requested && s->state.dispose_requested);
    CHECK(s->artifact && s->arena && !s->state.disposed);
    free(s->artifact);
    free(s->arena);
    s->artifact = s->arena = NULL;
    s->state.main_active = s->state.prepared = false;
    s->state.disposed = true; /* publish only after releasing private storage */
    step(f);
}

static void retained(fixture *f, const core_admission_record *r)
{
    CHECK(r->node && r->node->occupied && r->node->lifetime.bytes == 128);
    CHECK(r->node->lifetime.allocations == 2);
    CHECK(!core_lifetime_unclaimed(&r->node->lifetime));
    const core_node_execution *e = &f->nodes.executions[r->node - f->pool.nodes];
    CHECK(e->occupied && !e->worker_accepted);
    CHECK(r->node->prepare.phase == CORE_ACTION_EXECUTING);
    CHECK(e->worker.ticket == r->node->prepare.access.ticket);
    CHECK(!(r->evidence.objects[0] & CORE_OBJECT_CLEANED));
    CHECK(f->pool.reserved_bytes == 256);
}

static void unrelated(fixture *f, const core_admission_record *r, size_t index)
{
    CHECK(r->node == &f->pool.nodes[index] && r->node->instance.value == 2);
    CHECK(r->node->established && !r->node->lifetime.closing);
    CHECK(r->node->lifetime.bytes == 128 && f->slots[index].state.main_active);
    CHECK(!f->slots[index].state.cancel_requested);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED);
}

static void run(enum preparation_fault fault, bool cutoff)
{
    fixture f = {0};
    CHECK(core_admission_init(&f.pool, 128, 256));
    const uint64_t peers[] = {1};
    CHECK(core_admission_configure(&f.pool, 1, 99, peers, 1));
    f.nodes.admission = &f.pool;
    f.nodes.runtime = (core_node_runtime){.context = &f, .prepare = prepare,
        .lock = lock, .unlock = unlock};
    f.now = 10;
    const core_admission_record *target = add(&f, 1);
    const size_t index = (size_t)(target->node - f.pool.nodes);
    slot *s = &f.slots[index];
    core_reserved_node *reservation = target->node;
    /* Named preparation boundary positively reached: transfer accepted, but
     * no completion or allocation. No wall-clock sleeps or implicit warmup. */
    CHECK(s->artifact && !s->arena && !s->state.prepared);
    CHECK(reservation->prepare.phase == CORE_ACTION_EXECUTING);
    const core_admission_record *other = add(&f, 2);
    const size_t other_index = (size_t)(other->node - f.pool.nodes);
    complete_preparation(&f, other_index);
    start(&f, other_index, other);
    retained(&f, target);
    const core_admission_record *removal = NULL;
    if (fault == REMOVAL) removal = remove_node(&f, 3, 1);
    if (fault == EXPIRY) {
        f.now = target->deadline_us;
        step(&f);
        CHECK(target->evidence.objects[0] & CORE_OBJECT_EXPIRED);
    }
    CHECK(s->state.cancel_requested == (fault != NONE));
    retained(&f, target);
    unrelated(&f, other, other_index);
    printf("{\"boundary\":\"before_prepare_completion\",\"fault\":%u,"
           "\"cutoff\":%u,\"time\":%llu,\"reached_and_decision_applied\":true}\n",
           (unsigned)fault, cutoff, (unsigned long long)f.now);
    complete_preparation(&f, index); /* release only after decision above */
    if (fault == NONE) {
        start(&f, index, target); /* positive startup control */
        removal = remove_node(&f, 3, 1);
    } else {
        CHECK(!s->state.start_requested && !s->state.main_active);
        CHECK(!core_node_start_allowed(&f.nodes, index, f.now));
        CHECK(!(target->evidence.history & (CORE_HISTORY_DISPATCH | CORE_HISTORY_STARTED)));
    }
    /* Execution can stop without freeing private storage. Neither stopped
     * execution nor deadline passage is a disposal acknowledgement. */
    s->state.main_active = false;
    /* Named cleanup boundary: disposal commanded, but storage still owned. */
    CHECK(s->state.dispose_requested && s->state.cancel_requested && !s->state.disposed);
    CHECK(s->arena && s->artifact);
    retained(&f, target);
    const core_admission_record *closing = removal ? removal : target;
    const uint64_t deadline = closing->cleanup_deadline_us;
    CHECK(deadline > f.now);
    if (cutoff) {
        f.now = deadline - 1;
        step(&f);
        CHECK(core_report_outcome(&closing->evidence) == CORE_OUTCOME_PENDING);
        f.now = deadline;
        step(&f);
        CHECK(core_report_outcome(&closing->evidence) == CORE_OUTCOME_UNRESOLVED);
        CHECK(closing->evidence.history & CORE_HISTORY_CUTOFF);
        CHECK(!(closing->evidence.objects[0] & CORE_OBJECT_CLEANED));
    }
    retained(&f, target);
    unrelated(&f, other, other_index);
    CHECK(closing->cleanup_deadline_us == deadline);
    printf("{\"boundary\":\"before_disposal_completion\",\"fault\":%u,"
           "\"cutoff\":%u,\"time\":%llu,\"reserved_bytes\":%zu,"
           "\"outcome\":%u}\n", (unsigned)fault, cutoff,
           (unsigned long long)f.now, f.pool.reserved_bytes,
           (unsigned)core_report_outcome(&closing->evidence));
    dispose(&f, index);
    CHECK(target->node == NULL && !reservation->occupied);
    CHECK(core_lifetime_reclaimed(&reservation->lifetime) && f.pool.reserved_bytes == 128);
    CHECK(target->evidence.objects[0] & CORE_OBJECT_CLEANED);
    CHECK(core_report_outcome(&target->evidence) ==
          (fault == NONE ? CORE_OUTCOME_SUCCEEDED : CORE_OUTCOME_FAILED_CLEAN));
    if (removal) CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    step(&f); /* repeated observation must not reclaim another reservation */
    CHECK(f.pool.reserved_bytes == 128);
    unrelated(&f, other, other_index);
    const core_admission_record *other_removal = remove_node(&f, 4, 2);
    dispose(&f, other_index);
    CHECK(core_report_outcome(&other_removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(f.pool.reserved_bytes == 0);
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i)
        CHECK(!f.slots[i].artifact && !f.slots[i].arena && !f.nodes.executions[i].occupied);
    printf("{\"fault\":%u,\"cutoff\":%u,\"reclaimed\":true,\"reserved_bytes\":0}\n",
           (unsigned)fault, cutoff);
}

int main(void)
{
    for (enum preparation_fault fault = NONE; fault <= EXPIRY; ++fault) {
        run(fault, false);
        run(fault, true);
    }
    return 0;
}

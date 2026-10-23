#include "admission_fixture.h"
#include "kaos_node.h"

typedef struct {
    core_runtime_state states[CORE_MAX_NODE_RESERVATIONS];
    uint8_t *artifacts[CORE_MAX_NODE_RESERVATIONS];
    bool locked[CORE_MAX_NODE_RESERVATIONS];
    bool reject;
    unsigned calls;
} fake_runtime;

static bool prepare(void *context, size_t slot, uint8_t *artifact, size_t size,
                     const uint8_t *configuration, size_t configuration_size, size_t arena)
{
    fake_runtime *runtime = context;
    (void)configuration;
    CHECK(size == 16 && configuration_size == 0 && arena == 112);
    ++runtime->calls;
    if (runtime->reject) return false;
    runtime->states[slot] = (core_runtime_state){0};
    runtime->artifacts[slot] = artifact;
    return true;
}

static core_runtime_state *lock(void *context, size_t slot)
{
    fake_runtime *runtime = context;
    CHECK(!runtime->locked[slot]);
    runtime->locked[slot] = true;
    return &runtime->states[slot];
}

static void unlock(void *context, size_t slot)
{
    fake_runtime *runtime = context;
    CHECK(runtime->locked[slot]);
    runtime->locked[slot] = false;
}

typedef struct {
    core_admission admission;
    core_node_service service;
    fake_runtime runtime;
    uint8_t artifact[16];
} fixture;

static void init(fixture *f)
{
    memset(f, 0, sizeof(*f));
    fixture_pool(&f->admission, 1);
    f->service.admission = &f->admission;
    f->service.runtime = (core_node_runtime){.context = &f->runtime,
        .prepare = prepare, .lock = lock, .unlock = unlock};
}

static const core_admission_record *add(fixture *f, uint64_t id)
{
    fixture_request request = fixture_node(id, 10);
    const core_admission_record *r;
    CHECK(fixture_submit(&f->admission, &request, &r) == CORE_ADMIT_ACCEPTED);
    return r;
}

static const core_admission_record *remove_node(fixture *f, uint64_t id, uint64_t instance)
{
    fixture_request request = {.id = {id}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 10,
        .instance = {instance}, .allowance_us = 50}};
    const core_admission_record *r;
    CHECK(fixture_submit(&f->admission, &request, &r) == CORE_ADMIT_ACCEPTED);
    return r;
}

static void handoff(fixture *f, const core_admission_record *r)
{
    CHECK(core_lifetime_charge(&r->node->lifetime, sizeof(f->artifact)));
    CHECK(core_node_artifact(&f->service, r->id, f->artifact, sizeof(f->artifact), 15));
    CHECK(r->node->lifetime.bytes == 128 && r->node->lifetime.allocations == 2);
    CHECK(r->node->prepare.phase == CORE_ACTION_EXECUTING);
}

static void prepared(fixture *f)
{
    f->runtime.states[0].prepared = true;
    core_node_step(&f->service, 20);
    CHECK(f->runtime.states[0].start_requested);
}

static void started(fixture *f, const core_admission_record *r)
{
    prepared(f);
    f->runtime.states[0].main_active = true;
    f->runtime.states[0].init_pending = true;
    core_node_step(&f->service, 30);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(r->node->established && !f->runtime.states[0].init_pending);
    CHECK(core_node_next_deadline(&f->service) == UINT64_MAX);
}

static void disposed(fixture *f)
{
    core_runtime_state *state = &f->runtime.states[0];
    CHECK(state->cancel_requested && state->dispose_requested);
    state->main_active = false;
    state->disposed = true;
    f->runtime.artifacts[0] = NULL;
}

static void success_remove_and_cutoff(void)
{
    fixture f;
    init(&f);
    const core_admission_record *creation = add(&f, 1);
    CHECK(core_node_next_deadline(&f.service) == 110);
    handoff(&f, creation);
    started(&f, creation);
    const core_admission_record *removal = remove_node(&f, 2, 1);
    core_node_step(&f.service, 40);
    CHECK(!creation->node->established);
    CHECK(f.runtime.states[0].cancel_requested && f.runtime.states[0].dispose_requested);
    CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_PENDING);
    CHECK(creation->node->lifetime.bytes == 128);
    CHECK(core_node_next_deadline(&f.service) == 60);
    core_node_step(&f.service, 60);
    CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_UNRESOLVED);
    CHECK(core_report_outcome(&creation->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(!(removal->evidence.objects[0] & CORE_OBJECT_CLEANED));
    CHECK(core_node_next_deadline(&f.service) == UINT64_MAX);
    disposed(&f);
    core_node_step(&f.service, 61);
    CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_report_outcome(&creation->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(creation->node == NULL && f.admission.reserved_bytes == 0);
    CHECK(!f.service.executions[0].occupied);
}

static void marker_exit_race(void)
{
    fixture f;
    init(&f);
    const core_admission_record *r = add(&f, 1);
    handoff(&f, r);
    prepared(&f);
    f.runtime.states[0].init_pending = true;
    f.runtime.states[0].exited = true;
    core_node_step(&f.service, 30);
    CHECK(!(r->evidence.history & CORE_HISTORY_STARTED));
    CHECK(r->evidence.objects[0] & CORE_OBJECT_EXITED);
    CHECK(!r->node->established && core_node_next_deadline(&f.service) == 80);
    disposed(&f);
    core_node_step(&f.service, 31);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(f.admission.reserved_bytes == 0);
}

static void deadline_and_marker_guards(void)
{
    fixture f;
    init(&f);
    const core_admission_record *r = add(&f, 1);
    handoff(&f, r);
    f.runtime.states[0].init_pending = true; /* setup-time marker */
    core_node_step(&f.service, 16);
    CHECK(!f.runtime.states[0].init_pending && !(r->evidence.history & CORE_HISTORY_STARTED));
    prepared(&f);
    f.runtime.states[0].init_pending = true; /* prepared/configuring is not main */
    core_node_step(&f.service, 25);
    CHECK(!f.runtime.states[0].init_pending && !(r->evidence.history & CORE_HISTORY_STARTED));
    f.runtime.states[0].main_active = true;
    f.runtime.states[0].init_pending = true;
    core_node_step(&f.service, 110); /* equality is expired */
    CHECK(!(r->evidence.history & CORE_HISTORY_STARTED));
    CHECK(r->evidence.objects[0] & CORE_OBJECT_EXPIRED);
    CHECK(core_node_next_deadline(&f.service) == 160);
    core_node_step(&f.service, 150); /* repeating cancellation must not extend it */
    CHECK(core_node_next_deadline(&f.service) == 160);
    core_node_step(&f.service, 160);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_UNRESOLVED);
    disposed(&f);
    core_node_step(&f.service, 161);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);
}

static void removal_wins_preparation(void)
{
    fixture f;
    init(&f);
    const core_admission_record *r = add(&f, 1);
    handoff(&f, r);
    const core_admission_record *removal = remove_node(&f, 2, 1);
    f.runtime.states[0].prepared = true;
    f.runtime.states[0].main_active = true;
    f.runtime.states[0].init_pending = true;
    core_node_step(&f.service, 20);
    CHECK(!f.runtime.states[0].start_requested);
    CHECK(!(r->evidence.history & (CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH | CORE_HISTORY_STARTED)));
    disposed(&f);
    core_node_step(&f.service, 21);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_SUCCEEDED);
}

static void access_blocks_actual_disposal(void)
{
    fixture f;
    init(&f);
    const core_admission_record *r = add(&f, 1);
    handoff(&f, r);
    started(&f, r);
    core_access access;
    CHECK(core_access_acquire(&r->node->lifetime, &access));
    f.runtime.states[0].main_active = false;
    f.runtime.states[0].exited = true;
    f.runtime.states[0].fault = true;
    core_node_step(&f.service, 40);
    CHECK(f.runtime.states[0].cancel_requested && !f.runtime.states[0].dispose_requested);
    CHECK(r->node->lifetime.bytes == 128 && !r->node->established);
    CHECK((r->evidence.objects[0] & (CORE_OBJECT_EXITED | CORE_OBJECT_FAILED)) ==
          (CORE_OBJECT_EXITED | CORE_OBJECT_FAILED));
    CHECK(core_node_next_deadline(&f.service) == UINT64_MAX);
    CHECK(!(r->evidence.history & CORE_HISTORY_CLEANUP));
    CHECK(core_access_release(&r->node->lifetime, access));
    core_node_step(&f.service, 41);
    disposed(&f);
    core_node_step(&f.service, 42);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(r->node == NULL && f.admission.reserved_bytes == 0);
    const core_admission_record *removal = remove_node(&f, 2, 1);
    core_node_step(&f.service, 43);
    CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_SUCCEEDED);
}

static void staging_and_unknown_removal(void)
{
    fixture f;
    init(&f);
    const core_admission_record *r = add(&f, 1);
    CHECK(core_lifetime_charge(&r->node->lifetime, 16));
    core_access transfer;
    CHECK(core_access_acquire(&r->node->lifetime, &transfer));
    const core_admission_record *removal = remove_node(&f, 2, 1);
    core_node_step(&f.service, 20);
    CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_PENDING);
    CHECK(!core_node_artifact(&f.service, r->id, f.artifact, 16, 21));
    CHECK(core_access_release(&r->node->lifetime, transfer));
    CHECK(core_lifetime_uncharge(&r->node->lifetime, 16));
    core_node_step(&f.service, 22);
    CHECK(core_report_outcome(&removal->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    const core_admission_record *absent = remove_node(&f, 3, 90);
    core_node_step(&f.service, 23);
    CHECK(core_report_outcome(&absent->evidence) == CORE_OUTCOME_SUCCEEDED);
    CHECK(f.runtime.calls == 0);
    fixture_request delayed = fixture_node(90, 10);
    const core_admission_record *unchanged = NULL;
    CHECK(fixture_submit(&f.admission, &delayed, &unchanged) == CORE_ADMIT_CONFLICT);
    CHECK(unchanged == NULL); /* retained absent-instance removal still fences construction */
}

static void late_prepare_and_unreclaimed_exit(void)
{
    fixture f;
    init(&f);
    const core_admission_record *r = add(&f, 1);
    handoff(&f, r);
    f.runtime.states[0].prepared = true;
    core_node_step(&f.service, 110);
    CHECK(!f.runtime.states[0].start_requested);
    CHECK(!(r->evidence.objects[0] & CORE_OBJECT_READY));
    CHECK(!(r->evidence.history & CORE_HISTORY_COMMIT));
    disposed(&f);
    core_node_step(&f.service, 111);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);

    init(&f);
    r = add(&f, 1);
    handoff(&f, r);
    prepared(&f);
    f.runtime.states[0].exited = true; /* stopped execution still owns its allocations */
    core_node_step(&f.service, 30);
    core_node_step(&f.service, 80);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_UNRESOLVED);
    CHECK(r->node != NULL && r->node->lifetime.bytes == 128);
    CHECK(!(r->evidence.objects[0] & CORE_OBJECT_CLEANED));
    disposed(&f);
    core_node_step(&f.service, 81);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);
}

static void partial_failure_and_slot_reuse(void)
{
    fixture f;
    init(&f);
    const core_admission_record *r = add(&f, 1);
    handoff(&f, r);
    core_reserved_node *old_node = r->node;
    const core_access old_token = f.service.executions[0].worker;
    f.runtime.states[0].prepare_failed = true;
    core_node_step(&f.service, 20);
    CHECK(r->evidence.objects[0] & CORE_OBJECT_FAILED);
    CHECK(!(r->evidence.objects[0] & CORE_OBJECT_READY));
    disposed(&f);
    core_node_step(&f.service, 21);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);
    CHECK(core_admission_forget(&f.admission, r->id));
    const core_admission_record *next = add(&f, 2);
    CHECK(next->node == old_node);
    handoff(&f, next);
    CHECK(f.service.executions[0].worker.ticket > old_token.ticket);
    CHECK(!core_action_complete(&next->node->prepare, old_token, CORE_WORK_SUCCEEDED));
    CHECK(!core_access_release(&next->node->lifetime, old_token));
    started(&f, next);
    const uint8_t revision = next->evidence.revision;
    f.runtime.states[0].init_pending = true;
    core_node_step(&f.service, 31);
    CHECK(next->evidence.revision == revision);
}

static void dispatch_failure_retains_staging(void)
{
    fixture f;
    init(&f);
    const core_admission_record *r = add(&f, 1);
    CHECK(core_lifetime_charge(&r->node->lifetime, 16));
    f.runtime.reject = true;
    CHECK(!core_node_artifact(&f.service, r->id, f.artifact, 16, 15));
    CHECK(r->cleanup_selected && r->node->lifetime.bytes == 16);
    CHECK(r->node->prepare.phase == CORE_ACTION_EMPTY);
    CHECK(core_lifetime_unclaimed(&r->node->lifetime));
    CHECK(!f.service.executions[0].occupied && f.runtime.artifacts[0] == NULL);
    CHECK(core_lifetime_uncharge(&r->node->lifetime, 16));
    core_node_step(&f.service, 16);
    CHECK(core_report_outcome(&r->evidence) == CORE_OUTCOME_FAILED_CLEAN);
}

int main(void)
{
    success_remove_and_cutoff();
    marker_exit_race();
    deadline_and_marker_guards();
    removal_wins_preparation();
    access_blocks_actual_disposal();
    staging_and_unknown_removal();
    late_prepare_and_unreclaimed_exit();
    partial_failure_and_slot_reuse();
    dispatch_failure_retains_staging();
    puts("node: startup, cancellation, exit, deadlines, owned disposal and reuse passed");
    return 0;
}

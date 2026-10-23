/* Real admission/node engine + WAMR. Artifact staging is local and synthetic;
 * this fixture does not qualify transport, collector acknowledgement or Wi-Fi. */
#include "../../tests/admission_fixture.h"
#include "kaos_node.h"
#include "kaos_wamr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <assert.h>

static core_admission pool;
static core_node_service service;
static fixture_request request;
static uint8_t encoded[CORE_MAX_REQUEST_BYTES];
static const uint8_t *artifact;
static size_t artifact_size;

static uint64_t now(void) { return (uint64_t)esp_timer_get_time(); }

static void reset(core_node_runtime runtime)
{
    static const uint64_t peers[] = {1};
    memset(&pool, 0, sizeof(pool));
    memset(&service, 0, sizeof(service));
    const size_t budget = artifact_size + CORE_WAMR_ARENA_BYTES;
    assert(core_admission_init(&pool, budget, 2 * budget));
    assert(core_admission_configure(&pool, 1, 99, peers, 1));
    service.admission = &pool;
    service.runtime = runtime;
}

static const core_admission_record *submit(void)
{
    const size_t size = fixture_encode(&request, encoded);
    const core_admission_record *record = NULL;
    assert(core_request_submit(&pool, 99, encoded, size, now(), &record) == CORE_ADMIT_ACCEPTED);
    return record;
}

static const core_admission_record *add(uint8_t mode)
{
    request = fixture_node(1, 10);
    request.node.artifact_bytes = (uint32_t)artifact_size;
    request.node.required_bytes = (uint32_t)artifact_size + CORE_WAMR_ARENA_BYTES;
    request.node.configuration_size = 1;
    request.node.configuration[0] = mode;
    request.node.addition_allowance_us = 1000000;
    request.node.cleanup_allowance_us = 1000000;
    const core_admission_record *record = submit();
    uint8_t *copy = malloc(artifact_size);
    assert(copy);
    memcpy(copy, artifact, artifact_size);
    assert(core_lifetime_charge(&record->node->lifetime, artifact_size));
    assert(core_node_artifact(&service, record->id, copy, artifact_size, now()));
    return record;
}

static const core_admission_record *remove_node(uint64_t allowance)
{
    request = (fixture_request){.id = {2}, .scope = {
        .kind = CORE_REQUEST_REMOVE_NODE_NO_EDGES, .logical_node_id = 10,
        .instance = {1}, .allowance_us = allowance}};
    return submit();
}

static core_runtime_state runtime_state(void)
{
    core_runtime_state *state = service.runtime.lock(service.runtime.context, 0);
    assert(state);
    core_runtime_state result = *state;
    service.runtime.unlock(service.runtime.context, 0);
    return result;
}

/* Configuration evidence is a test observation; all admission/lifecycle
 * decisions still belong exclusively to the real node engine. */
static void await(const core_admission_record *record, core_outcome wanted,
                  bool configuration_error)
{
    const uint64_t limit = now() + 5000000;
    for (;;) {
        const uint64_t time = now();
        core_node_step(&service, time);
        if (configuration_error) {
            core_runtime_state state = runtime_state();
            if (state.errors_pending) {
                assert(state.configuration_active && !state.init_pending);
                return;
            }
        } else if (core_report_outcome(&record->evidence) == wanted) {
            return;
        }
        if (time >= limit) {
            printf("node fixture timeout: op=%llu wanted=%d actual=%d\n",
                   (unsigned long long)record->id.value, wanted,
                   core_report_outcome(&record->evidence));
            abort();
        }
        uint64_t deadline = core_node_next_deadline(&service);
        if (deadline > limit) deadline = limit;
        const uint64_t current = now();
        if (deadline <= current) continue; /* Process a timer already due. */
        TickType_t ticks = (TickType_t)((deadline - current + 999) / 1000 /
                                      portTICK_PERIOD_MS) + 1;
        (void)ulTaskNotifyTake(pdTRUE, ticks);
    }
}

static void reclaimed(const core_admission_record *record)
{
    assert(record->node == NULL && pool.reserved_bytes == 0);
    assert(!service.executions[0].occupied);
    assert(record->evidence.objects[0] & CORE_OBJECT_CLEANED);
    assert(runtime_state().disposed);
}

void p3_node_lifecycle(core_node_runtime runtime, const uint8_t *guest, size_t size)
{
    artifact = guest;
    artifact_size = size;
    reset(runtime);
    const core_admission_record *creation = add(0);
    await(creation, CORE_OUTCOME_SUCCEEDED, false);
    assert(creation->node->established);
    const core_admission_record *removal = remove_node(1000000);
    await(removal, CORE_OUTCOME_SUCCEEDED, false);
    reclaimed(creation);
    assert(core_report_outcome(&creation->evidence) == CORE_OUTCOME_SUCCEEDED);

    reset(runtime);
    creation = add(2);
    await(creation, CORE_OUTCOME_FAILED_CLEAN, false);
    assert(!(creation->evidence.history & CORE_HISTORY_STARTED));
    reclaimed(creation);

    reset(runtime);
    creation = add(1);
    await(creation, CORE_OUTCOME_PENDING, true);
    await(creation, CORE_OUTCOME_FAILED_CLEAN, false);
    assert(creation->evidence.objects[0] & CORE_OBJECT_EXPIRED);
    assert(!(creation->evidence.history & CORE_HISTORY_STARTED));
    reclaimed(creation);

    reset(runtime);
    creation = add(0);
    await(creation, CORE_OUTCOME_SUCCEEDED, false);
    core_access access;
    assert(core_access_acquire(&creation->node->lifetime, &access));
    removal = remove_node(100000);
    await(removal, CORE_OUTCOME_UNRESOLVED, false);
    core_runtime_state state = runtime_state();
    assert(state.cancel_requested && !state.dispose_requested && !state.disposed);
    assert(creation->node && pool.reserved_bytes != 0);
    assert(!(removal->evidence.objects[0] & CORE_OBJECT_CLEANED));
    assert(core_access_release(&creation->node->lifetime, access));
    await(removal, CORE_OUTCOME_SUCCEEDED, false);
    reclaimed(creation);
    puts("P3 ESP32 node lifecycle: add/remove, failed-clean, expiry, tracked-access cutoff PASS");
}

/* Physical ESP32 runtime qualification only: no protocol or Wi-Fi claim. */
#include "kaos_wamr.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const uint8_t guest_start[] __asm__("_binary_runtime_guest_wasm_start");
extern const uint8_t guest_end[] __asm__("_binary_runtime_guest_wasm_end");

void p3_node_lifecycle(core_node_runtime runtime, const uint8_t *guest, size_t size);

static core_node_runtime runtime;
static TaskHandle_t control;
static uint64_t worst_stop_us;
static uint64_t worst_control_gap_us;
enum observation { PREPARED, INIT, EXITED, DISPOSED, FAILED, ERROR_EVENT };

static void notify(void *unused)
{
    (void)unused;
    xTaskNotifyGive(control);
}

static core_runtime_state snapshot(size_t slot)
{
    core_runtime_state *state = runtime.lock(runtime.context, slot);
    assert(state);
    core_runtime_state value = *state;
    runtime.unlock(runtime.context, slot);
    return value;
}

static core_runtime_state await(size_t slot, enum observation wanted)
{
    const int64_t deadline = esp_timer_get_time() + 3000000;
    for (;;) {
        core_runtime_state state = snapshot(slot);
        bool ready = wanted == PREPARED ? state.prepared :
                     wanted == INIT ? state.init_pending :
                     wanted == EXITED ? state.exited :
                     wanted == DISPOSED ? state.disposed :
                     wanted == ERROR_EVENT ? state.errors_pending != 0 :
                     state.prepare_failed;
        if (ready) return state;
        const int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            printf("TIMEOUT slot=%u wanted=%d prepared=%d failed=%d exited=%d\n",
                   (unsigned)slot, wanted, state.prepared, state.prepare_failed, state.exited);
            abort();
        }
        /* Notifications remain pending between snapshot and wait. Round up
         * the finite wait, then recheck the absolute monotonic deadline. */
        TickType_t ticks = (TickType_t)((remaining + 999) / 1000 /
                                      portTICK_PERIOD_MS) + 1;
        (void)ulTaskNotifyTake(pdTRUE, ticks);
    }
}

static void prepare(size_t slot, uint8_t mode, size_t budget)
{
    const size_t size = (size_t)(guest_end - guest_start);
    uint8_t *copy = malloc(size);
    assert(copy);
    memcpy(copy, guest_start, size);
    assert(runtime.prepare(runtime.context, slot, copy, size, &mode, 1, budget));
}

static void start(size_t slot)
{
    core_runtime_state *state = runtime.lock(runtime.context, slot);
    state->start_deadline_us = (uint64_t)esp_timer_get_time() + 3000000;
    state->start_requested = true;
    runtime.unlock(runtime.context, slot);
}

static void cancel(size_t slot)
{
    core_runtime_state *state = runtime.lock(runtime.context, slot);
    state->cancel_requested = true;
    runtime.unlock(runtime.context, slot);
}

static void dispose(size_t slot)
{
    core_runtime_state *state = runtime.lock(runtime.context, slot);
    state->cancel_requested = true;
    state->dispose_requested = true;
    runtime.unlock(runtime.context, slot);
    assert(await(slot, DISPOSED).disposed);
}

static void stop(size_t slot)
{
    const int64_t before = esp_timer_get_time();
    cancel(slot);
    core_runtime_state state = await(slot, EXITED);
    assert(!state.main_active && !state.configuration_active && !state.fault);
    assert(!state.disposed);
    dispose(slot);
    uint64_t elapsed = (uint64_t)(esp_timer_get_time() - before);
    if (elapsed > worst_stop_us) worst_stop_us = elapsed;
}

static void heap_report(const char *phase)
{
    printf("%s internal free=%u minimum=%u largest=%u control_stack_free=%u\n",
           phase,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    TaskStatus_t tasks[16];
    const UBaseType_t count = uxTaskGetSystemState(tasks, 16, NULL);
    assert(count != 0);
    for (UBaseType_t i = 0; i < count; ++i)
        printf("task=%s priority=%u stack_free=%u\n", tasks[i].pcTaskName,
               (unsigned)tasks[i].uxCurrentPriority,
               (unsigned)tasks[i].usStackHighWaterMark);
}

static void cycle(bool report)
{
    /* Both instances must prepare without executing any configuration/main. */
    prepare(0, 4, CORE_WAMR_ARENA_BYTES);
    prepare(1, 0, CORE_WAMR_ARENA_BYTES);
    for (size_t slot = 0; slot < 2; ++slot) {
        core_runtime_state state = await(slot, PREPARED);
        assert(!state.configuration_active && !state.main_active &&
               !state.init_pending && !state.exited);
    }
    start(0);
    start(1);
    assert(await(0, INIT).main_active);
    assert(await(1, INIT).main_active);
    if (report) heap_report("two resident nodes");
    stop(0);
    assert(snapshot(1).main_active);
    stop(1);

    /* Guest-side ERROR proves configuration entered its compute/wait loop. */
    const uint8_t looping[] = {1, 10};
    for (size_t i = 0; i < sizeof(looping); ++i) {
        prepare(0, looping[i], CORE_WAMR_ARENA_BYTES);
        await(0, PREPARED);
        start(0);
        core_runtime_state state = await(0, ERROR_EVENT);
        assert(state.configuration_active && !state.init_pending);
        stop(0);
        assert(!snapshot(0).init_pending);
    }
    for (uint8_t mode = 2; mode <= 3; ++mode) {
        prepare(0, mode, CORE_WAMR_ARENA_BYTES);
        await(0, PREPARED);
        start(0);
        core_runtime_state state = await(0, EXITED);
        assert(state.fault && !state.init_pending && !state.main_active);
        dispose(0);
    }

    /* Normal/trapping exit keeps queued INIT as evidence, never liveness. */
    for (uint8_t mode = 5; mode <= 7; ++mode) {
        prepare(0, mode, CORE_WAMR_ARENA_BYTES);
        await(0, PREPARED);
        start(0);
        core_runtime_state state = await(0, EXITED);
        assert(!state.main_active && state.fault == (mode == 7));
        assert(state.init_pending == (mode != 5));
        dispose(0);
    }
    /* The worker rechecks authorization at actual call entry. */
    prepare(0, 0, CORE_WAMR_ARENA_BYTES);
    await(0, PREPARED);
    core_runtime_state *expired = runtime.lock(runtime.context, 0);
    expired->start_deadline_us = 1;
    expired->start_requested = true;
    runtime.unlock(runtime.context, 0);
    assert(!await(0, EXITED).init_pending);
    dispose(0);

    prepare(0, 0, CORE_WAMR_ARENA_BYTES);
    await(0, PREPARED);
    core_runtime_state *commands = runtime.lock(runtime.context, 0);
    commands->start_deadline_us = (uint64_t)esp_timer_get_time() + 3000000;
    commands->start_requested = true;
    commands->cancel_requested = true;
    runtime.unlock(runtime.context, 0);
    dispose(0);
    assert(!snapshot(0).init_pending && !snapshot(0).exited);

    prepare(0, 0, 128);
    assert(await(0, FAILED).prepare_failed);
    dispose(0);
}

void app_main(void)
{
    control = xTaskGetCurrentTaskHandle();
    vTaskPrioritySet(control, 3); /* Must preempt both priority-2 guest workers. */
    puts("P3 ESP32 restricted runtime qualification; no Wi-Fi or channel workload");
    assert(core_wamr_profile(guest_start, (size_t)(guest_end - guest_start)));
    assert(core_wamr_init(&runtime, notify, NULL));
    heap_report("before preparation");
    cycle(true); /* Warm all exercised paths before exact heap comparisons. */
    /* Both cores can be busy with noncooperative guests. The control task
     * must still wake on ticks and cancel them without guest cooperation. */
    prepare(0, 4, CORE_WAMR_ARENA_BYTES);
    prepare(1, 4, CORE_WAMR_ARENA_BYTES);
    await(0, PREPARED);
    await(1, PREPARED);
    start(0);
    start(1);
    await(0, INIT);
    await(1, INIT);
    for (unsigned i = 0; i < 10; ++i) {
        const int64_t tick_start = esp_timer_get_time();
        vTaskDelay(1);
        const uint64_t gap = (uint64_t)(esp_timer_get_time() - tick_start);
        if (gap > worst_control_gap_us) worst_control_gap_us = gap;
    }
    stop(0);
    stop(1);
    heap_report("warm baseline");
    const size_t baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const int64_t before = esp_timer_get_time();
    for (unsigned i = 0; i < 20; ++i) {
        cycle(false);
        assert(heap_caps_check_integrity_all(true));
        assert(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= baseline);
        /* Allow idle tasks to run; this is outside the measured lifecycle. */
        vTaskDelay(1);
    }
    heap_report("after repeated reclamation");
    printf("cycles=20 elapsed_us=%" PRId64 " worst_stop_and_dispose_us=%" PRIu64 "\n",
           esp_timer_get_time() - before, worst_stop_us);
    printf("two_compute_guests worst_control_tick_wait_us=%" PRIu64 " tick_ms=%u\n",
           worst_control_gap_us, (unsigned)portTICK_PERIOD_MS);
    p3_node_lifecycle(runtime, guest_start, (size_t)(guest_end - guest_start));
    heap_report("after node lifecycle");
    core_wamr_shutdown();
    heap_report("after runtime shutdown");
    puts("P3 ESP32 runtime qualification: PASS");
}

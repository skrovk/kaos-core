/* Capacity experiment: runtime slots only, not endpoint execution or admission. */
#define _POSIX_C_SOURCE 200809L
#include "kaos_wamr.h"
#include "kaos_admission.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern const unsigned char guest_start[] __asm__("_binary_guest_wasm_start");
extern const unsigned char guest_end[] __asm__("_binary_guest_wasm_end");
static TaskHandle_t control;
/* Include the candidate registry/report footprint even though this experiment
 * bypasses protocol admission and uses runtime slots directly. */
static volatile core_admission reserved_registry;
static void *endpoint_buffers[32];
#endif
static core_node_runtime runtime;
static const unsigned char *guest;
static size_t guest_size;
static uint64_t now(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
#endif
}
static void notify(void *unused)
{
    (void)unused;
#ifdef ESP_PLATFORM
    xTaskNotifyGive(control);
#endif
}
static core_runtime_state snapshot(size_t slot)
{
    core_runtime_state value = *runtime.lock(runtime.context, slot);
    runtime.unlock(runtime.context, slot);
    return value;
}
static core_runtime_state await(size_t slot, unsigned phase)
{
    const uint64_t end = now() + 3000000;
    for (;;) {
        core_runtime_state s = snapshot(slot);
        if (phase == 0 ? s.prepared || s.prepare_failed :
            phase == 1 ? s.init_pending || s.exited : s.disposed) return s;
        assert(now() < end);
#ifdef ESP_PLATFORM
        ulTaskNotifyTake(pdTRUE, 1);
#else
        const struct timespec pause = {0,1000000};
        nanosleep(&pause, NULL);
#endif
    }
}
static void dispose(size_t slot)
{
    core_runtime_state *s = runtime.lock(runtime.context, slot);
    s->cancel_requested = true;
    s->dispose_requested = true;
    runtime.unlock(runtime.context, slot);
    assert(await(slot, 2).disposed);
}
static bool start(size_t slot, size_t budget, uint8_t mode)
{
    unsigned char *copy = malloc(guest_size);
    assert(copy);
    memcpy(copy, guest, guest_size);
    assert(runtime.prepare(runtime.context, slot, copy, guest_size, &mode, 1, budget));
    if (!await(slot, 0).prepared) return false;
    core_runtime_state *s = runtime.lock(runtime.context, slot);
    s->start_requested = true;
    s->start_deadline_us = now() + 3000000;
    runtime.unlock(runtime.context, slot);
    s = NULL;
    core_runtime_state result = await(slot, 1);
    return result.init_pending && result.main_active && !result.fault;
}
static void run(void)
{
    assert(core_wamr_profile(guest, guest_size));
    assert(core_wamr_init(&runtime, notify, NULL));
    size_t low = 0, high = CORE_WAMR_ARENA_BYTES / 8;
    assert(start(0, high * 8, 0));
    dispose(0);
    while (low + 1 < high) {
        size_t mid = (low + high) / 2;
        bool good = start(0, mid * 8, 0);
        dispose(0);
        if (good) high = mid; else low = mid;
    }
    printf("minimum_fixture_arena=%u artifact=%u native_stack=%u slots=%u\n",
           (unsigned)(high * 8), (unsigned)guest_size,
           CORE_WAMR_NATIVE_STACK_BYTES, CORE_MAX_NODE_RESERVATIONS);
    /* Round up to KiB for a candidate profile; this is fixture headroom,
     * not evidence of worst-case application or endpoint memory demand. */
    size_t candidate = (high * 8 + 1023) / 1024 * 1024;
#ifdef ESP_PLATFORM
    /* Reserve the current conservative 256-byte x 4-message quota for all
     * 32 endpoints. These are touched buffers, not implemented endpoints. */
    for (size_t i = 0; i < 32; ++i) {
        endpoint_buffers[i] = malloc(CORE_ENDPOINT_BYTES);
        assert(endpoint_buffers[i]);
        memset(endpoint_buffers[i], 0xa5, CORE_ENDPOINT_BYTES);
    }
    printf("endpoint_buffer_reserve=%u endpoint_records=%u report_records=%u\n",
           32 * CORE_ENDPOINT_BYTES, CORE_MAX_ENDPOINT_RESERVATIONS, CORE_MAX_ADMISSION_RECORDS);
    const size_t baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i)
            assert(start(i, candidate, i % 2 ? 0 : 4));
        if (cycle == 0) {
            printf("concurrent=%u candidate_arena=%u\n", CORE_MAX_NODE_RESERVATIONS, (unsigned)candidate);
#ifdef ESP_PLATFORM
            printf("resident_free=%u largest=%u registry_bytes=%u\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)sizeof(reserved_registry));
#endif
        }
        for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
            assert(snapshot(i).main_active);
            dispose(i);
        }
#ifdef ESP_PLATFORM
        assert(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == baseline);
        vTaskDelay(1);
#endif
    }
#ifdef ESP_PLATFORM
    for (size_t i = 0; i < 32; ++i) free(endpoint_buffers[i]);
#endif
    core_wamr_shutdown();
    puts("capacity probe PASS");
}
#ifdef ESP_PLATFORM
void app_main(void)
{
    control = xTaskGetCurrentTaskHandle();
    vTaskPrioritySet(control, 3);
    reserved_registry.local_device = 1;
    guest = guest_start;
    guest_size = (size_t)(guest_end - guest_start);
    run();
}
#else
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fseek(f,0,SEEK_END)==0);
    long size = ftell(f);
    assert(size > 0 && fseek(f,0,SEEK_SET)==0);
    unsigned char *bytes = malloc((size_t)size);
    assert(bytes && fread(bytes,1,(size_t)size,f)==(size_t)size);
    fclose(f);
    guest = bytes;
    guest_size = (size_t)size;
    run();
    free(bytes);
    return 0;
}
#endif

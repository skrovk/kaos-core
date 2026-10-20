#define _POSIX_C_SOURCE 200809L

#include "kaos_wamr.h"
#include "kaos_admission.h"
#include "wasm_export.h"
#include "native_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef CORE_WAMR_TRACE_PREPARE
#include <stdio.h>
#endif

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

/* WAMR's allocator is process-wide. The restricted profile creates no guest
 * threads: every load/instantiate/call/unload runs on its owning worker, whose
 * TLS selects its bounded arena. Runtime-global initialization uses a separate
 * fixed arena. Individual free is intentionally deferred to whole-node disposal
 * so peak storage, including loader temporaries, has a single hard bound. */
typedef struct {
    uint8_t *bytes;
    size_t capacity, used;
#ifdef CORE_WAMR_TRACE_PREPARE
    /* Runtime and linear-memory payloads; bump usage also includes our header
     * and alignment padding. Counters belong to the owning allocator thread. */
    size_t requested[2], allocations[2];
#endif
} arena;
typedef union { max_align_t alignment; _Alignas(8) size_t size; } allocation;
static _Thread_local arena *allocation_arena;
static union { max_align_t alignment; _Alignas(8) uint8_t bytes[CORE_WAMR_GLOBAL_BYTES]; } global_storage;
static arena global_arena;

static void *bounded_malloc(mem_alloc_usage_t usage, unsigned int size)
{
    arena *a = allocation_arena;
    const size_t alignment = _Alignof(allocation);
    if (!a || !a->bytes || !size || size > a->capacity ||
        size > SIZE_MAX - sizeof(allocation) - alignment + 1) return NULL;
    const size_t total = (sizeof(allocation) + size + alignment - 1) & ~(alignment - 1);
    if (total > a->capacity - a->used) return NULL;
    allocation *header = (allocation *)(a->bytes + a->used);
    a->used += total;
    header->size = size;
#ifdef CORE_WAMR_TRACE_PREPARE
    const size_t category = usage == Alloc_For_LinearMemory ? 1 : 0;
    a->requested[category] += size;
    ++a->allocations[category];
#endif
    /* With usage-aware allocation, linear memory uses this arena rather
     * than a platform mmap allocation outside the node's charged budget. */
    if (usage == Alloc_For_LinearMemory) memset(header + 1, 0, size);
    return header + 1;
}

static void bounded_free(mem_alloc_usage_t usage, void *pointer)
{
    (void)usage;
    (void)pointer;
}

static void *bounded_realloc(mem_alloc_usage_t usage, bool full_size_mmaped,
                             void *pointer, unsigned int size)
{
    (void)full_size_mmaped;
    if (!pointer) return bounded_malloc(usage, size);
    if (!size) return NULL;
    allocation *header = (allocation *)pointer - 1;
    if (size <= header->size) return pointer;
    void *replacement = bounded_malloc(usage, size);
    if (replacement) memcpy(replacement, pointer, header->size);
    return replacement;
}

typedef struct {
    pthread_mutex_t mutex;
#ifdef ESP_PLATFORM
    StaticSemaphore_t wake_storage;
    SemaphoreHandle_t changed;
#else
    pthread_cond_t changed;
#endif
    pthread_t thread;
    core_runtime_state state;
    arena memory;
    uint8_t *artifact;
    size_t artifact_size;
    uint8_t configuration[CORE_MAX_CONFIGURATION_BYTES];
    size_t configuration_size;
    core_native_imports imports;
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t execution;
    wasm_function_inst_t configure, main;
    bool busy, prepare_requested, start_consumed, executing, init_seen, shutdown;
    bool channel_changed;
} runtime_slot;

static struct {
    runtime_slot slots[CORE_MAX_NODE_RESERVATIONS];
    void (*notify)(void *);
    void *context;
    core_wamr_channels channels;
    void *entry_context;
    bool (*entry_lock)(void *, size_t);
    void (*entry_unlock)(void *);
    bool initialized, channels_attached;
} service;

#ifdef CORE_WAMR_TRACE_PREPARE
static void trace_arena(const arena *a, const char *owner, size_t slot)
{
    const size_t count = a->allocations[0] + a->allocations[1];
    const size_t headers = count * sizeof(allocation);
    const size_t padding = a->used - a->requested[0] - a->requested[1] - headers;
    fprintf(stderr, "WAMR memory owner=%s slot=%zu capacity=%zu used=%zu unused=%zu "
            "runtime_payload=%zu linear_payload=%zu headers=%zu padding=%zu allocations=%zu\n",
            owner, slot, a->capacity, a->used, a->capacity - a->used,
            a->requested[0], a->requested[1], headers, padding, count);
}

static bool trace_global(wasm_module_inst_t instance, const char *name, uint32_t *value)
{
    wasm_global_inst_t global;
    if (!wasm_runtime_get_export_global_inst(instance, name, &global) ||
        global.kind != WASM_I32 || global.global_data == NULL) return false;
    memcpy(value, global.global_data, sizeof(*value));
    return true;
}
#endif

/* One worker waits on each mailbox. A retained semaphore token closes the
 * unlock/wait race on IDF, whose pthread timed waits use wall-clock time.
 * Predicates and deadlines are always checked again after a wakeup. */
static void signal_worker(runtime_slot *slot)
{
#ifdef ESP_PLATFORM
    (void)xSemaphoreGive(slot->changed);
#else
    pthread_cond_signal(&slot->changed);
#endif
}

static int wait_worker(runtime_slot *slot, const struct timespec *deadline)
{
#ifdef ESP_PLATFORM
    TickType_t ticks = portMAX_DELAY;
    if (deadline) {
        const int64_t remaining = (int64_t)deadline->tv_sec * 1000000 +
                                  deadline->tv_nsec / 1000 - esp_timer_get_time();
        if (remaining <= 0) return ETIMEDOUT;
        const uint64_t tick_us = (uint64_t)portTICK_PERIOD_MS * 1000;
        ticks = (TickType_t)(((uint64_t)remaining + tick_us - 1) / tick_us);
        /* Add a tick because a relative RTOS delay starts within a tick. */
        ++ticks;
    }
    pthread_mutex_unlock(&slot->mutex);
    const BaseType_t signalled = xSemaphoreTake(slot->changed, ticks);
    pthread_mutex_lock(&slot->mutex);
    return signalled == pdTRUE ? 0 : ETIMEDOUT;
#else
    return deadline ? pthread_cond_timedwait(&slot->changed, &slot->mutex, deadline) :
                      pthread_cond_wait(&slot->changed, &slot->mutex);
#endif
}

static void destroy_wakeup(runtime_slot *slot)
{
#ifdef ESP_PLATFORM
    vSemaphoreDelete(slot->changed);
#else
    pthread_cond_destroy(&slot->changed);
#endif
}

static void notify(void)
{
    if (service.notify) service.notify(service.context);
}

static runtime_slot *caller(wasm_exec_env_t execution)
{
    return wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(execution));
}

static int32_t guest_init(wasm_exec_env_t execution)
{
    runtime_slot *slot = caller(execution);
    pthread_mutex_lock(&slot->mutex);
    int32_t result = -1;
    if (slot->state.main_active && !slot->state.cancel_requested && !slot->init_seen) {
        slot->init_seen = true;
        slot->state.init_pending = true;
        result = 0;
    }
    pthread_mutex_unlock(&slot->mutex);
    if (result == 0) notify();
    return result;
}

static int32_t guest_error(wasm_exec_env_t execution)
{
    runtime_slot *slot = caller(execution);
    pthread_mutex_lock(&slot->mutex);
    int32_t result = -1;
    if (slot->executing && !slot->state.cancel_requested) {
        result = -2;
        if (slot->state.errors_pending < CORE_WAMR_ERROR_CAPACITY) {
            ++slot->state.errors_pending;
            result = 0;
        }
    }
    pthread_mutex_unlock(&slot->mutex);
    if (result == 0) notify();
    return result;
}

static int32_t guest_wait(wasm_exec_env_t execution, int32_t timeout_us)
{
    runtime_slot *slot = caller(execution);
    if (timeout_us < 0 || (uint32_t)timeout_us > CORE_WAMR_MAX_WAIT_US) return -1;
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return -1;
    deadline.tv_nsec += (long)timeout_us * 1000;
    deadline.tv_sec += deadline.tv_nsec / 1000000000;
    deadline.tv_nsec %= 1000000000;
    pthread_mutex_lock(&slot->mutex);
    int32_t result = -1;
    for (;;) {
        if (slot->state.cancel_requested || slot->shutdown) break;
        const bool poll = slot->state.main_active && service.channels_attached;
        slot->channel_changed = false;
        pthread_mutex_unlock(&slot->mutex);
        /* Registry -> mailbox is the sole lock order. Checking the retained
         * event outside our lock also allows closure to wake a waiting guest. */
        const bool ready = poll && service.channels.event_ready(service.channels.context,
                                                (size_t)(slot - service.slots));
        pthread_mutex_lock(&slot->mutex);
        if (slot->state.cancel_requested || slot->shutdown) break;
        if (ready) { result = 0; break; }
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) break;
        if (!timeout_us || now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            result = 1;
            break;
        }
        /* A change between the readiness check and reacquiring this lock
         * causes another poll. Later changes signal the atomic unlock/wait. */
        if (slot->channel_changed) continue;
        const int status = wait_worker(slot, &deadline);
        if (status != 0 && status != ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&slot->mutex);
    return result;
}

static bool channel_permitted(runtime_slot *slot)
{
    pthread_mutex_lock(&slot->mutex);
    const bool permitted = service.channels_attached && slot->state.main_active &&
                           !slot->state.cancel_requested && !slot->shutdown;
    pthread_mutex_unlock(&slot->mutex);
    return permitted;
}

/* WAMR's validate_app_addr sets a trapping exception. Invalid arguments to
 * copied I/O are recoverable errors, so use the nontrapping range query.
 * This profile has one nongrowing, unshared memory; pointers remain stable
 * for this bounded native call. Zero length permits an end-of-memory pointer. */
static bool guest_buffer(runtime_slot *slot, uint32_t offset, uint32_t size,
                         uint8_t **out)
{
    uint64_t end;
    if (!wasm_runtime_get_app_addr_range(slot->instance, 0, NULL, &end) ||
        offset > end || size > end - offset) return false;
    uint8_t *base = wasm_runtime_addr_app_to_native(slot->instance, 0);
    if (!base) return false;
    *out = base + offset;
    return true;
}

bool core_native_buffer(wasm_exec_env_t execution, uint32_t offset,
                        uint32_t size, uint8_t **out)
{
    runtime_slot *slot = caller(execution);
    return slot && guest_buffer(slot, offset, size, out);
}

bool core_native_delay(wasm_exec_env_t execution, uint32_t duration_us)
{
    runtime_slot *slot = caller(execution);
    if (!slot || duration_us > CORE_WAMR_MAX_WAIT_US) return false;
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return false;
    deadline.tv_nsec += (long)duration_us * 1000;
    deadline.tv_sec += deadline.tv_nsec / 1000000000;
    deadline.tv_nsec %= 1000000000;
    pthread_mutex_lock(&slot->mutex);
    bool complete = false;
    while (slot->executing && !slot->state.cancel_requested && !slot->shutdown) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) break;
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            complete = true;
            break;
        }
        const int status = wait_worker(slot, &deadline);
        if (status != 0 && status != ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&slot->mutex);
    return complete;
}

static bool guest_channel(runtime_slot *slot, uint32_t offset, uint64_t channel[3])
{
    uint8_t *bytes;
    if (!guest_buffer(slot, offset, 24, &bytes)) return false;
    for (size_t i = 0; i < 3; ++i) {
        channel[i] = 0;
        for (unsigned j = 0; j < 8; ++j)
            channel[i] |= (uint64_t)bytes[i * 8 + j] << (j * 8);
    }
    return true;
}

static int32_t guest_put(wasm_exec_env_t execution, uint32_t channel_offset,
                         uint32_t data_offset, uint32_t size)
{
    runtime_slot *slot = caller(execution);
    if (!channel_permitted(slot)) return -1;
    uint64_t channel[3];
    uint8_t *data;
    if (!guest_channel(slot, channel_offset, channel) ||
        !guest_buffer(slot, data_offset, size, &data)) return -4;
    return service.channels.put(service.channels.context,
                (size_t)(slot - service.slots), channel, data, size);
}

static int32_t guest_get(wasm_exec_env_t execution, uint32_t channel_offset,
                         uint32_t data_offset, uint32_t capacity)
{
    runtime_slot *slot = caller(execution);
    if (!channel_permitted(slot)) return -1;
    uint64_t channel[3];
    uint8_t *data;
    if (!guest_channel(slot, channel_offset, channel) ||
        !guest_buffer(slot, data_offset, capacity, &data)) return -4;
    return service.channels.get(service.channels.context,
                (size_t)(slot - service.slots), channel, data, capacity);
}

static int32_t guest_event_get(wasm_exec_env_t execution, uint32_t data_offset,
                               uint32_t capacity)
{
    runtime_slot *slot = caller(execution);
    if (!channel_permitted(slot)) return -1;
    uint8_t *data;
    if (capacity < 32) return -5;
    if (!guest_buffer(slot, data_offset, capacity, &data)) return -4;
    uint64_t channel[3];
    uint32_t permissions;
    const int32_t result = service.channels.event_get(service.channels.context,
                (size_t)(slot - service.slots), channel, &permissions);
    if (result != 0) return result;
    for (size_t i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 8; ++j)
            data[i * 8 + j] = (uint8_t)(channel[i] >> (j * 8));
    for (unsigned i = 0; i < 4; ++i) data[24 + i] = (uint8_t)(permissions >> (i * 8));
    memset(data + 28, 0, 4);
    return 0;
}

static NativeSymbol symbols[] = {
    {"init", (void *)guest_init, "()i", NULL},
    {"error", (void *)guest_error, "()i", NULL},
    {"event_wait", (void *)guest_wait, "(i)i", NULL},
    {"put", (void *)guest_put, "(iii)i", NULL},
    {"get", (void *)guest_get, "(iii)i", NULL},
    {"event_get", (void *)guest_event_get, "(ii)i", NULL}
};

/* The trusted registry currently contains only integer-offset/copy interfaces.
 * Validate the real loaded Wasm type, rather than trusting the guest's name or
 * WAMR's process-global linkage. Every check precedes instantiation. */
static bool import_signature(wasm_func_type_t type, const char *expected)
{
    if (!type || !expected || *expected++ != '(') return false;
    uint32_t parameters = 0;
    while (*expected == 'i') {
        if (parameters >= wasm_func_type_get_param_count(type) ||
            wasm_func_type_get_param_valkind(type, parameters) != WASM_I32)
            return false;
        ++parameters;
        ++expected;
    }
    if (*expected++ != ')' || parameters != wasm_func_type_get_param_count(type))
        return false;
    if (*expected == '\0') return wasm_func_type_get_result_count(type) == 0;
    return expected[0] == 'i' && expected[1] == '\0' &&
           wasm_func_type_get_result_count(type) == 1 &&
           wasm_func_type_get_result_valkind(type, 0) == WASM_I32;
}

static bool validate_loaded_imports(runtime_slot *slot)
{
    const int32_t count = wasm_runtime_get_import_count(slot->module);
    if (count < 0 || count > 6 + CORE_MAX_NATIVE_IMPORTS) return false;
    for (int32_t i = 0; i < count; ++i) {
        wasm_import_t import = {0};
        wasm_runtime_get_import_type(slot->module, i, &import);
        if (!import.linked || import.kind != WASM_IMPORT_EXPORT_KIND_FUNC ||
            !import.module_name || !import.name) return false;
        const char *expected = NULL;
        if (strcmp(import.module_name, "kaos") == 0) {
            for (size_t j = 0; j < sizeof(symbols) / sizeof(symbols[0]); ++j)
                if (strcmp(import.name, symbols[j].symbol) == 0)
                    expected = symbols[j].signature;
        } else if (strcmp(import.module_name, CORE_NATIVE_MODULE) == 0 &&
                   core_native_granted(&slot->imports, import.name)) {
            expected = core_native_signature(import.name);
        }
        if (!import_signature(import.u.func_type, expected)) return false;
    }
    return true;
}

static bool signature(wasm_module_inst_t instance, wasm_function_inst_t function,
                      uint32_t parameters, uint32_t results)
{
    if (!function || wasm_func_get_param_count(function, instance) != parameters ||
        wasm_func_get_result_count(function, instance) != results) return false;
    wasm_valkind_t types[2];
    wasm_func_get_param_types(function, instance, types);
    for (uint32_t i = 0; i < parameters; ++i) if (types[i] != WASM_I32) return false;
    wasm_func_get_result_types(function, instance, types);
    return !results || types[0] == WASM_I32;
}

static void allocate_runtime_arena(runtime_slot *slot)
{
#ifdef ESP_PLATFORM
    /* IDF malloc guarantees four-byte alignment; WAMR's heap requires eight. */
    slot->memory.bytes = heap_caps_aligned_alloc(_Alignof(allocation),
        slot->memory.capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    slot->memory.bytes = malloc(slot->memory.capacity);
#endif
}

static bool load_wasm_module(runtime_slot *slot, char *error, size_t error_size)
{
    slot->module = wasm_runtime_load(slot->artifact, (uint32_t)slot->artifact_size,
                                     error, error_size);
    return slot->module != NULL;
}

static bool instantiate_wasm_module(runtime_slot *slot, char *error, size_t error_size)
{
    /* These fixed stacks/heaps are inside the bounded arena, and the
     * profile rejects paths that execute guest code during instantiate. */
    slot->instance = wasm_runtime_instantiate(slot->module, 8192, 4096, error, error_size);
    return slot->instance != NULL;
}

static bool resolve_guest_entrypoints(runtime_slot *slot)
{
    wasm_runtime_set_custom_data(slot->instance, slot);
    slot->execution = wasm_runtime_create_exec_env(slot->instance, 8192);
    slot->configure = wasm_runtime_lookup_function(slot->instance, "kaos_configure");
    slot->main = wasm_runtime_lookup_function(slot->instance, "kaos_main");
    return slot->execution && signature(slot->instance, slot->configure, 2, 1) &&
           signature(slot->instance, slot->main, 0, 0);
}

static void publish_runtime_preparation_result(runtime_slot *slot, bool good)
{
    /* Handles remain private during preparation: cancellation can only
     * suppress startup. No untrusted function executes in this phase. */
    pthread_mutex_lock(&slot->mutex);
    slot->state.prepared = good;
    slot->state.prepare_failed = !good;
    pthread_mutex_unlock(&slot->mutex);
    notify();
}

static void prepare_wamr_slot(runtime_slot *slot)
{
    const bool profile = core_wamr_profile_with_imports(slot->artifact,
                                               slot->artifact_size, &slot->imports);
    bool good = profile;
    allocate_runtime_arena(slot);
    allocation_arena = &slot->memory;
    bool thread_environment = wasm_runtime_init_thread_env();
#ifdef CORE_WAMR_TRACE_PREPARE
    const size_t thread_payload = slot->memory.requested[0];
#endif
    good = good && slot->memory.bytes && thread_environment;
    char error[128] = {0};
    if (good) good = load_wasm_module(slot, error, sizeof(error));
    if (good) good = validate_loaded_imports(slot);
#ifdef CORE_WAMR_TRACE_PREPARE
    const size_t loader_payload = slot->memory.requested[0] - thread_payload;
#endif
    if (good) good = instantiate_wasm_module(slot, error, sizeof(error));
#ifdef CORE_WAMR_TRACE_PREPARE
    const size_t instance_payload = slot->memory.requested[0] - thread_payload - loader_payload;
#endif
    if (good) good = resolve_guest_entrypoints(slot);
#ifdef CORE_WAMR_TRACE_PREPARE
    trace_arena(&slot->memory, "node", (size_t)(slot - service.slots));
    uint32_t heap_base = 0, data_end = 0;
    const bool layout = slot->instance != NULL &&
        trace_global(slot->instance, "__heap_base", &heap_base) &&
        trace_global(slot->instance, "__data_end", &data_end);
    fprintf(stderr, "WAMR stages slot=%zu thread_payload=%zu loader_payload=%zu "
            "instance_payload=%zu exec_payload=%zu exec_stack_requested=8192 "
            "app_heap_requested=4096 layout=%u heap_base=%u data_end=%u artifact=%zu\n",
            (size_t)(slot - service.slots), thread_payload, loader_payload, instance_payload,
            slot->memory.requested[0] - thread_payload - loader_payload - instance_payload,
            (unsigned)layout, (unsigned)heap_base, (unsigned)data_end, slot->artifact_size);
    /* Qualification-only, on the worker before publishing its result. A null
     * arena identifies heap allocation failure; a live arena plus used/capacity
     * and WAMR's error identifies failures inside the charged node budget. */
    if (!good)
        fprintf(stderr, "WAMR prepare failed slot=%zu arena=%p used=%zu capacity=%zu "
                "profile=%u thread=%u module=%u instance=%u exec=%u error=%s\n",
                (size_t)(slot - service.slots), (void *)slot->memory.bytes,
                slot->memory.used, slot->memory.capacity, (unsigned)profile,
                (unsigned)thread_environment, slot->module != NULL,
                slot->instance != NULL, slot->execution != NULL, error);
#endif
    publish_runtime_preparation_result(slot, good);
}

static bool begin_call(runtime_slot *slot, bool main)
{
    const bool guarded = service.entry_lock != NULL;
    const bool allowed = !guarded || service.entry_lock(service.entry_context,
        (size_t)(slot - service.slots));
    pthread_mutex_lock(&slot->mutex);
    /* Lock contention must not turn an old timestamp into entry permission. */
    struct timespec now;
    const bool clock_valid = clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
                             now.tv_sec >= 0 && now.tv_nsec >= 0 &&
                             (uint64_t)now.tv_sec <=
                             (UINT64_MAX - (uint64_t)now.tv_nsec / 1000) / 1000000;
    const uint64_t now_us = clock_valid ? (uint64_t)now.tv_sec * 1000000 +
                                         (uint64_t)now.tv_nsec / 1000 : UINT64_MAX;
    const bool permitted = allowed && !slot->state.cancel_requested && !slot->shutdown &&
                           !slot->state.exited && slot->state.prepared &&
                           now_us < slot->state.start_deadline_us;
    if (permitted) {
        slot->executing = true;
        slot->state.main_active = main;
        slot->state.configuration_active = !main;
    }
    pthread_mutex_unlock(&slot->mutex);
    if (guarded) service.entry_unlock(service.entry_context);
    if (permitted) notify();
    return permitted;
}

static void run(runtime_slot *slot)
{
    bool good = false;
    if (begin_call(slot, false)) {
        void *copy = NULL;
        uint32_t address = 0;
        if (slot->configuration_size) {
            address = wasm_runtime_module_malloc(slot->instance,
                        (uint32_t)slot->configuration_size, &copy);
            if (address) memcpy(copy, slot->configuration, slot->configuration_size);
        }
        uint32_t arguments[2] = {address, (uint32_t)slot->configuration_size};
        good = (!slot->configuration_size || address) &&
               wasm_runtime_call_wasm(slot->execution, slot->configure, 2, arguments) &&
               arguments[0] == 0;
        pthread_mutex_lock(&slot->mutex);
        slot->executing = false;
        slot->state.configuration_active = false;
        pthread_mutex_unlock(&slot->mutex);
    }
    if (good && begin_call(slot, true)) {
        uint32_t result = 0;
        good = wasm_runtime_call_wasm(slot->execution, slot->main, 0, &result);
    }
    pthread_mutex_lock(&slot->mutex);
    slot->executing = false;
    slot->state.configuration_active = false;
    slot->state.main_active = false;
    slot->state.exited = true;
    slot->state.fault = !good && !slot->state.cancel_requested && !slot->shutdown;
    pthread_mutex_unlock(&slot->mutex);
    notify();
}

static void dispose(runtime_slot *slot)
{
    if (slot->execution) wasm_runtime_destroy_exec_env(slot->execution);
    if (slot->instance) wasm_runtime_deinstantiate(slot->instance);
    if (slot->module) wasm_runtime_unload(slot->module);
    wasm_runtime_destroy_thread_env();
#ifdef CORE_WAMR_TRACE_PREPARE
    /* Bump usage is monotonic: this is the whole-lifecycle peak, including
     * allocations made after prepare and memory logically freed by WAMR. */
    trace_arena(&slot->memory, "node-dispose", (size_t)(slot - service.slots));
#endif
    free(slot->memory.bytes);
    free(slot->artifact);
    allocation_arena = NULL;
    pthread_mutex_lock(&slot->mutex);
    slot->execution = NULL;
    slot->instance = NULL;
    slot->module = NULL;
    slot->configure = NULL;
    slot->main = NULL;
    slot->memory = (arena){0};
    slot->artifact = NULL;
    slot->artifact_size = 0;
    slot->imports = (core_native_imports){0};
    slot->busy = false;
    slot->state.prepared = false;
    slot->state.main_active = false;
    slot->state.disposed = true;
    pthread_mutex_unlock(&slot->mutex);
    notify();
}

static void *worker(void *context)
{
    runtime_slot *slot = context;
#ifdef ESP_PLATFORM
    vTaskPrioritySet(NULL, CORE_WAMR_WORKER_PRIORITY);
#endif
    pthread_mutex_lock(&slot->mutex);
    for (;;) {
        if (slot->prepare_requested) {
            slot->prepare_requested = false;
            pthread_mutex_unlock(&slot->mutex);
            prepare_wamr_slot(slot);
            pthread_mutex_lock(&slot->mutex);
        } else if (slot->busy && (slot->state.dispose_requested || slot->shutdown)) {
            pthread_mutex_unlock(&slot->mutex);
            dispose(slot);
            pthread_mutex_lock(&slot->mutex);
        } else if (slot->state.prepared && slot->state.start_requested &&
                   !slot->start_consumed && !slot->state.cancel_requested) {
            slot->start_consumed = true;
            pthread_mutex_unlock(&slot->mutex);
            run(slot);
            pthread_mutex_lock(&slot->mutex);
        } else if (slot->shutdown) {
            break;
        } else {
            (void)wait_worker(slot, NULL);
        }
    }
    pthread_mutex_unlock(&slot->mutex);
    return NULL;
}

static bool queue_prepare_with_imports(void *context, size_t index,
                          uint8_t *artifact, size_t size,
                          const uint8_t *configuration, size_t configuration_size,
                          size_t arena_bytes, const core_native_imports *imports)
{
    (void)context;
    if (!core_native_imports_valid(imports) ||
        !service.initialized || index >= CORE_MAX_NODE_RESERVATIONS || !artifact ||
        !size || size > CORE_MAX_ARTIFACT_BYTES ||
        configuration_size > CORE_MAX_CONFIGURATION_BYTES ||
        (configuration_size && !configuration) || !arena_bytes ||
        arena_bytes > CORE_WAMR_ARENA_BYTES) return false;
    runtime_slot *slot = &service.slots[index];
    pthread_mutex_lock(&slot->mutex);
    if (slot->busy || slot->shutdown) { pthread_mutex_unlock(&slot->mutex); return false; }
    slot->state = (core_runtime_state){0};
    slot->memory = (arena){.capacity = arena_bytes};
    slot->artifact = artifact;
    slot->artifact_size = size;
    slot->configuration_size = configuration_size;
    slot->imports = *imports;
    if (configuration_size) memcpy(slot->configuration, configuration, configuration_size);
    slot->busy = true;
    slot->prepare_requested = true;
    slot->start_consumed = false;
    slot->init_seen = false;
    slot->channel_changed = false;
    signal_worker(slot);
    pthread_mutex_unlock(&slot->mutex);
    return true;
}

static bool queue_prepare(void *context, size_t index, uint8_t *artifact, size_t size,
                          const uint8_t *configuration, size_t configuration_size,
                          size_t arena_bytes)
{
    const core_native_imports empty = {0};
    return queue_prepare_with_imports(context, index, artifact, size,
                            configuration, configuration_size, arena_bytes, &empty);
}

bool core_wamr_attach_entry_guard(void *context,
    bool (*entry_lock)(void *, size_t), void (*entry_unlock)(void *))
{
    if (!service.initialized || service.entry_lock || !entry_lock || !entry_unlock) return false;
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i)
        pthread_mutex_lock(&service.slots[i].mutex);
    bool idle = true;
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) idle &= !service.slots[i].busy;
    if (idle) {
        service.entry_context = context;
        service.entry_lock = entry_lock;
        service.entry_unlock = entry_unlock;
    }
    for (size_t i = CORE_MAX_NODE_RESERVATIONS; i > 0; --i)
        pthread_mutex_unlock(&service.slots[i - 1].mutex);
    return idle;
}

bool core_wamr_attach_channels(const core_wamr_channels *channels)
{
    if (!service.initialized || service.channels_attached || !channels ||
        !channels->put || !channels->get || !channels->event_get ||
        !channels->event_ready) return false;
    /* Startup-only configuration. Lock all slots to reject active instances
     * and publish the immutable callback table before any worker can use it. */
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i)
        pthread_mutex_lock(&service.slots[i].mutex);
    bool idle = true;
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i)
        idle = idle && !service.slots[i].busy;
    if (idle) {
        service.channels = *channels;
        service.channels_attached = true;
    }
    for (size_t i = CORE_MAX_NODE_RESERVATIONS; i > 0; --i)
        pthread_mutex_unlock(&service.slots[i - 1].mutex);
    return idle;
}

void core_wamr_channel_changed(size_t index)
{
    if (!service.initialized || index >= CORE_MAX_NODE_RESERVATIONS) return;
    runtime_slot *slot = &service.slots[index];
    pthread_mutex_lock(&slot->mutex);
    slot->channel_changed = true;
    signal_worker(slot);
    pthread_mutex_unlock(&slot->mutex);
}

static core_runtime_state *lock(void *context, size_t index)
{
    (void)context;
    if (!service.initialized || index >= CORE_MAX_NODE_RESERVATIONS) return NULL;
    runtime_slot *slot = &service.slots[index];
    pthread_mutex_lock(&slot->mutex);
    return &slot->state;
}

static void unlock(void *context, size_t index)
{
    (void)context;
    runtime_slot *slot = &service.slots[index];
    /* executing is published under this same lock before each guest call and
     * cleared after return. Disposal runs only after execution has returned. */
    if (slot->state.cancel_requested && slot->executing)
        wasm_runtime_terminate(slot->instance);
    signal_worker(slot);
    pthread_mutex_unlock(&slot->mutex);
}

bool core_wamr_init(core_node_runtime *out, void (*callback)(void *), void *context)
{
    if (!out || service.initialized) return false;
    memset(&service, 0, sizeof(service));
    global_arena = (arena){.bytes = global_storage.bytes, .capacity = sizeof(global_storage.bytes)};
    allocation_arena = &global_arena;
    RuntimeInitArgs args = {0};
    args.mem_alloc_type = Alloc_With_Allocator;
    args.mem_alloc_option.allocator.malloc_func = (void *)bounded_malloc;
    args.mem_alloc_option.allocator.realloc_func = (void *)bounded_realloc;
    args.mem_alloc_option.allocator.free_func = (void *)bounded_free;
    args.max_thread_num = 1;
    args.native_module_name = "kaos";
    args.native_symbols = symbols;
    args.n_native_symbols = sizeof(symbols) / sizeof(symbols[0]);
    const bool initialized = wasm_runtime_full_init(&args);
#ifdef CORE_WAMR_TRACE_PREPARE
    trace_arena(&global_arena, "global", 0);
    fprintf(stderr, "WAMR records service=%zu arena=%zu trace_counters=%zu "
            "trace_total=%zu allocation_header=%zu allocation_alignment=%zu\n",
            sizeof(service), sizeof(arena), 4 * sizeof(size_t),
            (CORE_MAX_NODE_RESERVATIONS + 1) * 4 * sizeof(size_t),
            sizeof(allocation), (size_t)_Alignof(allocation));
#endif
    if (!initialized) return false;
    if (!core_native_register()) goto fail_runtime;
    service.notify = callback;
    service.context = context;
    pthread_attr_t attributes;
#ifndef ESP_PLATFORM
    pthread_condattr_t conditions;
#endif
    if (pthread_attr_init(&attributes) != 0) goto fail_runtime;
    if (pthread_attr_setstacksize(&attributes, CORE_WAMR_NATIVE_STACK_BYTES) != 0)
        goto fail_attributes;
#ifndef ESP_PLATFORM
    if (pthread_condattr_init(&conditions) != 0) goto fail_attributes;
    if (pthread_condattr_setclock(&conditions, CLOCK_MONOTONIC) != 0)
        goto fail_conditions;
#endif
    size_t count = 0;
    for (; count < CORE_MAX_NODE_RESERVATIONS; ++count) {
        runtime_slot *slot = &service.slots[count];
        if (pthread_mutex_init(&slot->mutex, NULL) != 0) break;
#ifdef ESP_PLATFORM
        slot->changed = xSemaphoreCreateBinaryStatic(&slot->wake_storage);
        if (slot->changed == NULL) {
#else
        if (pthread_cond_init(&slot->changed, &conditions) != 0) {
#endif
            pthread_mutex_destroy(&slot->mutex);
            break;
        }
        if (pthread_create(&slot->thread, &attributes, worker, slot) != 0) {
            destroy_wakeup(slot);
            pthread_mutex_destroy(&slot->mutex);
            break;
        }
    }
#ifndef ESP_PLATFORM
    pthread_condattr_destroy(&conditions);
#endif
    pthread_attr_destroy(&attributes);
    if (count != CORE_MAX_NODE_RESERVATIONS) {
        for (size_t i = 0; i < count; ++i) {
            runtime_slot *slot = &service.slots[i];
            pthread_mutex_lock(&slot->mutex);
            slot->shutdown = true;
            signal_worker(slot);
            pthread_mutex_unlock(&slot->mutex);
            pthread_join(slot->thread, NULL);
            destroy_wakeup(slot);
            pthread_mutex_destroy(&slot->mutex);
        }
        goto fail_runtime;
    }
    service.initialized = true;
    *out = (core_node_runtime){.prepare = queue_prepare, .lock = lock, .unlock = unlock,
                               .prepare_with_imports = queue_prepare_with_imports};
    return true;

#ifndef ESP_PLATFORM
fail_conditions:
    pthread_condattr_destroy(&conditions);
#endif
fail_attributes:
    pthread_attr_destroy(&attributes);
fail_runtime:
    wasm_runtime_destroy();
    allocation_arena = NULL;
    return false;
}

void core_wamr_shutdown(void)
{
    if (!service.initialized) return;
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        runtime_slot *slot = &service.slots[i];
        pthread_mutex_lock(&slot->mutex);
        slot->shutdown = true;
        slot->state.cancel_requested = true;
        if (slot->executing) wasm_runtime_terminate(slot->instance);
        signal_worker(slot);
        pthread_mutex_unlock(&slot->mutex);
    }
    for (size_t i = 0; i < CORE_MAX_NODE_RESERVATIONS; ++i) {
        runtime_slot *slot = &service.slots[i];
        pthread_join(slot->thread, NULL);
        destroy_wakeup(slot);
        pthread_mutex_destroy(&slot->mutex);
    }
    allocation_arena = &global_arena;
#ifdef CORE_WAMR_TRACE_PREPARE
    trace_arena(&global_arena, "global-before-destroy", 0);
#endif
    wasm_runtime_destroy();
#ifdef CORE_WAMR_TRACE_PREPARE
    trace_arena(&global_arena, "global-after-destroy", 0);
#endif
    allocation_arena = NULL;
    service.initialized = false;
}

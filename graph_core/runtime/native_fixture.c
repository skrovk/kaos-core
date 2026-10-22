#include "native_internal.h"

#include <string.h>

atomic_uint core_native_fixture_calls;
atomic_uint core_native_fixture_active;

/* Synthetic qualification sensor, shared host/ESP ABI. Sensor 0 immediately
 * returns 23000 millidegrees Celsius; sensor 1 simulates a cancellable 100 ms
 * acquisition and returns 24000. No physical sensor availability is implied. */
static int32_t fixture_temperature_read(wasm_exec_env_t execution, uint32_t sensor, uint32_t offset,
                                        uint32_t capacity)
{
    uint8_t *output;
    if (sensor > 1 || capacity != 4 || !core_native_buffer(execution, offset, capacity, &output))
        return -4;
    atomic_fetch_add(&core_native_fixture_calls, 1);
    atomic_fetch_add(&core_native_fixture_active, 1);
    int32_t result = -1;
    if (core_native_delay(execution, sensor ? 100000 : 0)) {
        const uint32_t value = sensor ? 24000 : 23000;
        for (unsigned i = 0; i < 4; ++i)
            output[i] = (uint8_t)(value >> (8 * i));
        result = 4;
    }
    atomic_fetch_sub(&core_native_fixture_active, 1);
    return result;
}

/* WAMR may sort this array during registration, so it cannot be const. */
static NativeSymbol registry[] = {
    {"fixture_temperature_read", (void *)fixture_temperature_read, "(iii)i", NULL}};

bool core_native_register(void)
{
    return wasm_runtime_register_natives(CORE_NATIVE_MODULE, registry,
                                         sizeof(registry) / sizeof(registry[0]));
}

const char *core_native_signature(const char *name)
{
    for (size_t i = 0; i < sizeof(registry) / sizeof(registry[0]); ++i)
        if (strcmp(name, registry[i].symbol) == 0)
            return registry[i].signature;
    return NULL;
}

bool core_native_granted(const core_native_imports *imports, const char *name)
{
    for (size_t i = 0; i < imports->count; ++i)
        if (strcmp(name, imports->names[i]) == 0)
            return true;
    return false;
}

bool core_native_grants_registered(const core_native_imports *imports)
{
    if (!core_native_imports_valid(imports))
        return false;
    for (size_t i = 0; i < imports->count; ++i)
        if (!core_native_signature(imports->names[i]))
            return false;
    return true;
}

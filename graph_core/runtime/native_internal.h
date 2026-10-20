#ifndef KAOS_NATIVE_INTERNAL_H
#define KAOS_NATIVE_INTERNAL_H

#include "kaos_native.h"
#include "wasm_export.h"
#include <stdatomic.h>

/* Fixed trusted registry. Registration is linkage only; the profile and loaded
 * module checks enforce each node's separate grant before instantiation. */
bool core_native_register(void);
const char *core_native_signature(const char *name);
bool core_native_grants_registered(const core_native_imports *imports);
bool core_native_granted(const core_native_imports *imports, const char *name);

/* Helpers attribute the call through WAMR's trusted instance custom data.
 * They never retain guest addresses; disposal runs after the call returns. */
bool core_native_buffer(wasm_exec_env_t execution, uint32_t offset,
                        uint32_t size, uint8_t **out);
bool core_native_delay(wasm_exec_env_t execution, uint32_t duration_us);

/* Synthetic sensor fixture only: observability for its ABI/cleanup checks. */
extern atomic_uint core_native_fixture_calls;
extern atomic_uint core_native_fixture_active;

#endif

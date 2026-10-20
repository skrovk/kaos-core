#ifndef KAOS_NATIVE_H
#define KAOS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Additional imports have one explicit, fixed module identity. Standard kaos
 * lifecycle/channel imports remain implicit. Grants belong to one node instance
 * and never name addresses, resources or dynamically loaded host code. */
#define CORE_NATIVE_MODULE "kaos_native"
#define CORE_MAX_NATIVE_IMPORTS 4
#define CORE_MAX_NATIVE_IMPORT_NAME_BYTES 32

typedef struct {
    uint8_t count;
    char names[CORE_MAX_NATIVE_IMPORTS][CORE_MAX_NATIVE_IMPORT_NAME_BYTES + 1];
} core_native_imports;

/* Structural validation is shared by admission and runtime preparation.
 * Registry availability is checked separately before instantiation. */
static inline bool core_native_imports_valid(const core_native_imports *imports)
{
    if (!imports || imports->count > CORE_MAX_NATIVE_IMPORTS) return false;
    for (size_t i = 0; i < imports->count; ++i) {
        size_t length = 0;
        while (length <= CORE_MAX_NATIVE_IMPORT_NAME_BYTES &&
               imports->names[i][length] != '\0') {
            const unsigned char c = (unsigned char)imports->names[i][length];
            if (c < 0x21 || c > 0x7e) return false;
            ++length;
        }
        if (!length || length > CORE_MAX_NATIVE_IMPORT_NAME_BYTES) return false;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(imports->names[i], imports->names[j]) == 0) return false;
    }
    return true;
}

#endif

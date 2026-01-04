#ifndef WAMR_WRAP_H
#define WAMR_WRAP_H

#include "wasm_export.h"


typedef wasm_exec_env_t exec_env_t;
typedef wasm_module_t module_t;
typedef wasm_module_inst_t module_inst_t;

typedef NativeSymbol export_symbol_t;
typedef RuntimeInitArgs wamr_init_args_t;

#endif
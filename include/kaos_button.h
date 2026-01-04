#include "iot_button.h"
#include "button_gpio.h"
#include "kaos_errors.h"
#include "wasm_export.h"

uint32_t register_button(wasm_exec_env_t exec_env, int32_t id);
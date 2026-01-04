#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "wasm_export.h"

#define HIGH (1)
#define LOW (0)

#define GPIO_NUM_27 (27)
#define GPIO_NUM_26 (26)
#define GPIO_NUM_25 (25)

#define RGB_GREEN_PIN GPIO_NUM_27
#define BLUE_PIN GPIO_NUM_26
#define RED_PIN GPIO_NUM_25

int setup_LED();
void delay(wasm_exec_env_t exec_env, int time);

int claim_red(wasm_exec_env_t exec_env);
int claim_blue(wasm_exec_env_t exec_env);
int claim_green(wasm_exec_env_t exec_env);

int surrender_red(wasm_exec_env_t exec_env);
int surrender_blue(wasm_exec_env_t exec_env);
int surrender_green(wasm_exec_env_t exec_env);
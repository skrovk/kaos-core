#include "led.h"

#include "esp_timer.h"
#include "esp_log.h"

void delay(wasm_exec_env_t exec_env, int time) {
    vTaskDelay(time / portTICK_PERIOD_MS);
}

int claim_red(wasm_exec_env_t exec_env) {
    gpio_set_level(RED_PIN, HIGH);
    // ESP_LOGE("LATENCY_MEASUREMENT_END", "%lli", get_time_us());
    return 0;
}

int claim_blue(wasm_exec_env_t exec_env) {
    gpio_set_level(BLUE_PIN, HIGH);
    return 0;
}

int claim_green(wasm_exec_env_t exec_env) {
    gpio_set_level(RGB_GREEN_PIN, HIGH);
    return 0;
}

int surrender_red(wasm_exec_env_t exec_env) {
    gpio_set_level(RED_PIN, LOW);
    // ESP_LOGE("LATENCY_MEASUREMENT_END", "%lli", get_time_us());
    return 0;
}

int surrender_blue(wasm_exec_env_t exec_env) {
    gpio_set_level(BLUE_PIN, LOW);
    return 0;
}

int surrender_green(wasm_exec_env_t exec_env) {
    gpio_set_level(RGB_GREEN_PIN, LOW);
    return 0;
}


int setup_LED() {
    gpio_reset_pin(GPIO_NUM_25);
    gpio_reset_pin(GPIO_NUM_26);
    gpio_reset_pin(GPIO_NUM_27);

    gpio_set_direction(GPIO_NUM_25, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_27, GPIO_MODE_OUTPUT);

    return 0;
}
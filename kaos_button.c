#include "kaos_button.h"
#include "esp_log.h"
#include "esp_err.h"

#include "kaos_monitor.h"
#include "container_mgr.h"


void button_single_click_cb(void *arg, void *data) {
        ESP_LOGI("button", "BUTTON_SINGLE_CLICK");
        submit_event_from_ISR(EVENT_ISR, data, sizeof(isr_data_t)); 
}


uint32_t register_button(wasm_exec_env_t exec_env, int32_t id) {
    module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    module_registry_t *registry = get_registry_by_inst(module_inst);

    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = 25,
        .active_level = 0,
    };

    button_handle_t gpio_btn = NULL;
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &gpio_btn);
    if (NULL == gpio_btn) {
        ESP_LOGE("button", "Button create failed");
        return 27;
    }

    // // create adc button
    // const button_config_t btn_cfg = {0};
    // button_adc_config_t btn_adc_cfg = {
    //     .unit_id = ADC_UNIT_1,
    //     .adc_channel = 18,
    //     .button_index = 0,
    //     .min = 100,
    //     .max = 400,
    // };

    // button_handle_t adc_btn = NULL;
    // esp_err_t ret = iot_button_new_adc_device(&btn_cfg, &btn_adc_cfg, &adc_btn);
    // if(NULL == adc_btn) {
    //     ESP_LOGE(TAG, "Button create failed");
    // }

    // TODO: Destruction handling
    isr_data_t *b_isr_data = calloc(1, sizeof(isr_data_t));
    b_isr_data->registry = registry;
    b_isr_data->id = id;

    err = iot_button_register_cb(gpio_btn, BUTTON_SINGLE_CLICK, NULL, button_single_click_cb, (void *) b_isr_data);

    if (err != ESP_OK) {
        ESP_LOGE("button", "Button register cb failed");
        return 27;
    }

    return 0;
}

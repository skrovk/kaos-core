#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "kaos_errors.h"
#include "port.h"

// TODO: check tyep correctness with FreeRTOS API
queue_handle_t create_queue(uint32_t queue_length, uint32_t item_size) {
    return xQueueCreate(queue_length, item_size);
}

kaos_error_t delete_queue(queue_handle_t queue) {
    vQueueDelete(queue);
    return KaosSuccess;
}

kaos_error_t send_to_queue(queue_handle_t queue, void *item, uint32_t timeout_ms) {
    if (!xQueueSend(queue, item, timeout_ms / portTICK_PERIOD_MS)) {
        ESP_LOGE(__FUNCTION__, "Queue full");
        return KaosQueueFullError;
    }
    return KaosSuccess;
}

kaos_error_t send_to_queue_from_ISR(queue_handle_t queue, void *item) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    
    if (!xQueueSendFromISR(queue, item, &higher_priority_task_woken)) {
        ESP_LOGE(__FUNCTION__, "Queue full");
        return KaosQueueFullError;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);

    return KaosSuccess;
}

kaos_error_t receive_from_queue(queue_handle_t queue, void *item, uint32_t timeout_ms) {
    if (!xQueueReceive(queue, item, timeout_ms / portTICK_PERIOD_MS)) {
        ESP_LOGE(__FUNCTION__, "Queue empty");
        return KaosQueueEmptyError;
    }
    return KaosSuccess;
}

kaos_error_t receive_from_queue_ISR(queue_handle_t queue, void *item) {
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (!xQueueReceiveFromISR(queue, item, &higher_priority_task_woken)) {
        ESP_LOGE(__FUNCTION__, "Queue empty");
        return KaosQueueEmptyError;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);

    return KaosSuccess;
}

typedef struct kaos_timer_t esp_timer_handle_t;
typedef struct kaos_timer_args_t esp_timer_create_args_t;

int64_t get_time_us() {
    return esp_timer_get_time();
}

kaos_error_t create_timer(kaos_timer_t *timer_dest, kaos_timer_args_t *args) {
    esp_err_t esp_err =  esp_timer_create(args, timer_dest);
    if (esp_err != ESP_OK) {
        ESP_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
        return KaosTimerError;
    }
}

kaos_error_t start_timer(kaos_timer_t timer, uint64_t timeout_us, bool repeat) {
    esp_err_t esp_err;
    if (repeat) {
        esp_err = esp_timer_start_periodic(timer, timeout_us);
    } else {
        esp_err = esp_timer_start_once(timer, timeout_us);
    }

    if (esp_err != ESP_OK) {
        ESP_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
        return KaosTimerError;
    }

    return KaosSuccess;
}

kaos_error_t stop_timer(kaos_timer_t timer) {
    esp_err_t esp_err = esp_timer_stop(timer);
    if (esp_err != ESP_OK) {
        ESP_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
        return KaosTimerError;
    }

    return KaosSuccess;
}
kaos_error_t delete_timer(kaos_timer_t timer) {
    esp_err_t esp_err = esp_timer_delete(timer);
    if (esp_err != ESP_OK) {
        ESP_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
        return KaosTimerError;
    }

    return KaosSuccess;
}

bool timer_is_active(kaos_timer_t timer) {
    return esp_timer_is_active(timer);
}

ESP_LOGI
ESP_LOGE -> printf
ESP_LOGW

start_server



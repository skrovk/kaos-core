struct queue {QueueHandle_t queue; };
struct kaos_timer { esp_timer_handle_t timer; };
struct task_handle { TaskHandle_t handle; };
struct kaos_timer_args { esp_timer_create_args_t args; };

kaos_error_t create_task(void callback(void *), char *name, uint32_t stack_depth, void *arg, uint32_t priority, task_handle_t **out_handle) {
    TaskHandle_t handle;
    if (xTaskCreate(callback, name, stack_depth, arg, priority, &handle) != pdPASS) {
        KAOS_LOGE(__FUNCTION__, "Task creation failed");
        return KaosTaskCreationError;
    }
    *out_handle = calloc(1, sizeof(task_handle_t));
    (*out_handle)->handle = handle;
    return KaosSuccess;
}


kaos_error_t notify_task_give(task_handle_t *task_handle) {
    xTaskNotifyGive(task_handle->handle);
    return KaosSuccess;
}

// TODO: check tyep correctness with FreeRTOS API
queue_t *create_queue(uint32_t queue_length, uint32_t item_size) {
    queue_t *q = calloc(1, sizeof(queue_t));
    if (!q) {
        KAOS_LOGE(__FUNCTION__, "Queue allocation failed");
        return NULL;
    }
    q->queue = xQueueCreate(queue_length, item_size);
    if (!q->queue) {
        KAOS_LOGE(__FUNCTION__, "Queue creation failed");
        free(q);
        return NULL;
    }
    return q;
}

kaos_error_t delete_queue(queue_t* queue) {
    vQueueDelete(queue->queue);
    free(queue);
    return KaosSuccess;
}

kaos_error_t send_to_queue(queue_t* queue, void *item, uint32_t timeout_ms) {
    if (!xQueueSend(queue->queue, item, timeout_ms / portTICK_PERIOD_MS)) {
        KAOS_LOGE(__FUNCTION__, "Queue full");
        return KaosQueueFullError;
    }
    return KaosSuccess;
}

kaos_error_t send_to_queue_from_ISR(queue_t *queue, void *item) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    
    if (!xQueueSendFromISR(queue->queue, item, &higher_priority_task_woken)) {
        KAOS_LOGE(__FUNCTION__, "Queue full");
        return KaosQueueFullError;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);

    return KaosSuccess;
}

kaos_error_t receive_from_queue(queue_t *queue, void *item, uint32_t timeout_ms) {
    if (!xQueueReceive(queue->queue, item, timeout_ms / portTICK_PERIOD_MS)) {
        KAOS_LOGE(__FUNCTION__, "Queue empty");
        return KaosQueueEmptyError;
    }
    return KaosSuccess;
}

kaos_error_t receive_from_queue_ISR(queue_t * queue, void *item) {
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (!xQueueReceiveFromISR(queue->queue, item, &higher_priority_task_woken)) {
        KAOS_LOGE(__FUNCTION__, "Queue empty");
        return KaosQueueEmptyError;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);

    return KaosSuccess;
}

kaos_error_t messages_available(queue_t * queue, uint32_t *out_n_messages) {
    if (uxQueueMessagesWaiting(queue->queue) > 0) {
        *out_n_messages = uxQueueMessagesWaiting(queue->queue);
        return KaosSuccess;
    } else {
        *out_n_messages = 0;
        return KaosNothingToReceive;
    }
}

int64_t get_time_us() {
    return esp_timer_get_time();
}

kaos_error_t create_timer(kaos_timer_handle_t **timer_dest, kaos_timer_args_t *args) {
    kaos_timer_handle_t *timer = calloc(1, sizeof(kaos_timer_handle_t));
    if (!timer) {
        KAOS_LOGE(__FUNCTION__, "Timer allocation failed");
        return KaosMemoryAllocationError;
    }
    *timer_dest = timer;
    esp_err_t esp_err =  esp_timer_create(&(args->args), &(timer->timer));
    if (esp_err != ESP_OK) {
        KAOS_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
        return KaosTimerError;
    }

    return KaosSuccess;
}

kaos_error_t start_timer(kaos_timer_handle_t *timer, uint64_t timeout_us, bool repeat) {
    esp_err_t esp_err;
    if (repeat) {
        esp_err = esp_timer_start_periodic(timer->timer, timeout_us);
    } else {
        esp_err = esp_timer_start_once(timer->timer, timeout_us);
    }

    if (esp_err != ESP_OK) {
        KAOS_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
        return KaosTimerError;
    }

    return KaosSuccess;
}

kaos_error_t stop_timer(kaos_timer_handle_t *timer) {
    esp_err_t esp_err = esp_timer_stop(timer->timer);
    if (esp_err != ESP_OK) {
        KAOS_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
        return KaosTimerError;
    }

    return KaosSuccess;
}
kaos_error_t delete_timer(kaos_timer_handle_t *timer) {
    esp_err_t esp_err = esp_timer_delete(timer->timer);
    if (esp_err != ESP_OK) {
        ESP_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
        return KaosTimerError;
    }
    free(timer);

    return KaosSuccess;
}

bool timer_is_active(kaos_timer_handle_t *timer) {
    return esp_timer_is_active(timer->timer);
}

/* Allocate and initialize platform-specific timer args (ESP-IDF).
 * Returns NULL on allocation failure. Caller must free with
 * `kaos_timer_args_free()`.
 */
kaos_timer_args_t *kaos_timer_args_init(void (*callback)(void *), void *arg, const char *name) {
    esp_timer_create_args_t *a = calloc(1, sizeof(esp_timer_create_args_t));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    
    a->callback = callback;
    a->arg = arg;
    if (name) {
        size_t n = strlen(name) + 1;
        char *s = calloc(n, 1);
        if (!s) { free(a); return NULL; }
        memcpy(s, name, n);
        a->name = s;
    } else {
        a->name = NULL;
    }
    return (kaos_timer_args_t *)a;
}

void kaos_timer_args_free(kaos_timer_args_t *args) {
    if (!args) return;
    esp_timer_create_args_t *a = (esp_timer_create_args_t *)args;
    if (a->name) free((void *)a->name);
    free(a);
}


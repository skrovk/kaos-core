queue_t * create_queue(uint32_t queue_length, uint32_t item_size) {
    return xQueueCreate(queue_length, item_size);
}

kaos_error_t delete_queue(queue_t * queue) {
    vQueueDelete(queue);
    return KaosSuccess;
}

kaos_error_t send_to_queue(queue_t * queue, void *item, uint32_t timeout_ms) {
    if (!xQueueSend(queue, item, timeout_ms / portTICK_PERIOD_MS)) {
        ESP_LOGE(__FUNCTION__, "Queue full");
        return KaosQueueFullError;
    }
    return KaosSuccess;
}

kaos_error_t send_to_queue_from_ISR(queue_t * queue, void *item) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    
    if (!xQueueSendFromISR(queue, item, &higher_priority_task_woken)) {
        ESP_LOGE(__FUNCTION__, "Queue full");
        return KaosQueueFullError;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);

    return KaosSuccess;
}

kaos_error_t receive_from_queue(queue_t * queue, void *item, uint32_t timeout_ms) {
    if (!xQueueReceive(queue, item, timeout_ms / portTICK_PERIOD_MS)) {
        ESP_LOGE(__FUNCTION__, "Queue empty");
        return KaosQueueEmptyError;
    }
    return KaosSuccess;
}


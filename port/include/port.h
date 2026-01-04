typedef int queue_handle_t;

queue_handle_t create_queue(uint32_t queue_length, uint32_t item_size);
kaos_error_t delete_queue(queue_handle_t queue);
kaos_error_t send_to_queue(queue_handle_t queue, void *item, uint32_t timeout_ms);
kaos_error_t send_to_queue_from_ISR(queue_handle_t queue, void *item);
kaos_error_t receive_from_queue(queue_handle_t queue, void *item, uint32_t timeout_ms); 

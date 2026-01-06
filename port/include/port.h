#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stdbool.h>
#include "../../include/kaos_errors.h"

typedef struct queue queue_t;
typedef struct task_handle task_handle_t;
typedef struct kaos_timer kaos_timer_handle_t;
typedef struct kaos_timer_args kaos_timer_args_t;

kaos_error_t create_task(void callback(void *), char *name, uint32_t stack_depth, void *arg, uint32_t priority, task_handle_t **out_handle);
kaos_error_t notify_task_give(task_handle_t *task_handle);

queue_t *create_queue(uint32_t queue_length, uint32_t item_size);
kaos_error_t delete_queue(queue_t * queue);
kaos_error_t send_to_queue(queue_t * queue, void *item, uint32_t timeout_ms);
kaos_error_t send_to_queue_from_ISR(queue_t * queue, void *item);
kaos_error_t receive_from_queue(queue_t * queue, void *item, uint32_t timeout_ms); 
kaos_error_t messages_available(queue_t * queue, uint32_t *out_n_messages);

kaos_error_t receive_from_queue_ISR(queue_t * queue, void *item);

kaos_timer_args_t *kaos_timer_args_init(void (*callback)(void *), void *arg, const char *name);
void kaos_timer_args_free(kaos_timer_args_t *args);

int64_t get_time_us();
kaos_error_t create_timer(kaos_timer_handle_t **timer_dest, kaos_timer_args_t *args);
kaos_error_t start_timer(kaos_timer_handle_t *timer, uint64_t timeout_us, bool repeat);

kaos_error_t stop_timer(kaos_timer_handle_t *timer);
kaos_error_t delete_timer(kaos_timer_handle_t *timer);
bool timer_is_active(kaos_timer_handle_t *timer);
#endif // PORT_H

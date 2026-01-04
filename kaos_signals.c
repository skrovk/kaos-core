#include <inttypes.h>
#include <pthread.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"


#include "container_mgr.h"
#include "kaos_errors.h"
#include "kaos_types_shared.h"
#include "kaos_signals.h"
#include "kaos_monitor.h"
#include "wamr_wrap.h"
#include "wasm_export.h"


#define TIMEOUT_QUEUE (0)

char *TAG = "signals";
#define KAOS_MAX_MODULE_N CONFIG_KAOS_MAX_MODULE_N
// No of int32_t to contain flags for all required pool flags 
// 2<<5 = 32

#define SEGMENT_SHIFT_SZ   5
#define KAOS_ID_POOL_SZ (((KAOS_MAX_MODULE_N * KAOS_SIG_QUEUE_SIZE) >> SEGMENT_SHIFT_SZ) + 1)

#define KAOS_SUSPEND_TIMEOUT CONFIG_KAOS_SUSPEND_TIMEOUT
// Each container maintains a set of it's own signal queues
// The monitor task maintains a set of running timers for each signal sent
// When container sends a response, an event is submitted to the main event queue with container handle and signal response data


signal_id_t signal_id_pool[KAOS_ID_POOL_SZ] = {0};
signal_id_t await_response_signal_ids[KAOS_MAX_MODULE_N * KAOS_SIG_QUEUE_SIZE] = {0};

// TODO: signed v unsigned - check correctness
static signal_id_t assign_signal_id(void) {
    for (int32_t i = 0; i < KAOS_ID_POOL_SZ; i++) {
        for (int32_t j = 0; j < (1u << SEGMENT_SHIFT_SZ); j++) {
            if (!(signal_id_pool[i] & (1u << j))) {
                signal_id_pool[i] = signal_id_pool[i] | (int32_t) (1u << j);
                signal_id_t id = (i << SEGMENT_SHIFT_SZ) + j;
                ESP_LOGI(__FUNCTION__, "Assigned id %d", id);
                return id;
            }
        }
    }
    return -1;
}

kaos_error_t release_signal_id(signal_id_t signal_id) {
    signal_id_t pool_segment =  signal_id_pool[signal_id >> SEGMENT_SHIFT_SZ];
    signal_id_t segment_id = signal_id & ((1u << SEGMENT_SHIFT_SZ) - 1u);
    if (pool_segment & (1u << segment_id)) {
        signal_id_pool[signal_id >> SEGMENT_SHIFT_SZ] = pool_segment ^ (1u << segment_id);  
        return 0;
    }
    ESP_LOGE(__FUNCTION__, "Signal id not in pool");
    return -1;
}


bool is_response(kaos_signal_type_t signal) {
    return signal & 0x1;
}


queue_t init_sig_queue_to_container(void) {
    return create_queue(KAOS_SIG_QUEUE_SIZE, sizeof(kaos_signal_msg_t));
}


void destroy_sig_queues(module_registry_t *registry) {
    queue_t queue = get_signal_queue_to_container(registry);
    if (queue) delete_queue(queue);
}

// Signal timer cancelled immediately before put into queue
void signal_timer_expired(void *arg) {
    submit_event(EVENT_SIGNAL_TIMER_EXPIRED, arg, sizeof(signal_id_t));
}


// TODO: Chnage data field to statuc buffer so that change of ownership is not necessary
// Send signal to container signal queue. Sets the signal_id field in kaos_signal_msg_t structure at signal_msg
kaos_error_t sig_to_container(module_registry_t *registry, kaos_signal_msg_t *signal_msg) {
    if (!registry) {
        ESP_LOGW(TAG, "%s: Module registry not found", __FUNCTION__);
        return 1;
    }

    if (!is_response(signal_msg->signal_type)) {
        signal_msg->signal_id = assign_signal_id();
    }
    
    if (!send_to_queue(get_signal_queue_to_container(registry), signal_msg, TIMEOUT_QUEUE / portTICK_PERIOD_MS)) {
        ESP_LOGW(TAG, "%s: Signal queue for module %s full", __FUNCTION__, get_module_name(registry));
        return KaosQueueFullError;
    }

    return KaosSuccess;
}

// TODO: Error check submit event
int32_t sig_response_to_kaos(exec_env_t exec_env, int32_t signal_type, signal_id_t signal_id, uint64_t data) {
    // If the message is a signal response, cancel signal timer before event is emitted
    // Signal timer should be handled by the operation responsible. If the timer expires before  EVENT_SIGNAL_RECEIVED is 
    // processed by event queue, it should still precede it and operation should not be awaiting the event anymore - TEST
    kaos_signal_msg_t *event_buffer = calloc(1, sizeof(kaos_signal_msg_t));
    *event_buffer = (kaos_signal_msg_t) {
        .signal_type = (kaos_signal_type_t) signal_type | (kaos_signal_type_t) 1,
        .signal_id = (signal_id_t) signal_id
    };

    memcpy(&(event_buffer->data), &data, sizeof(uint64_t));
    submit_event(EVENT_SIGNAL_RECEIVED, event_buffer, sizeof(kaos_signal_msg_t));
    return KaosSuccess;
}

// Signal type e.g. KAOS_SIGNAL_TERMINATE | KAOS_SIGNAL_RESPONSE to send response to a terminate signal
// TODO: Need bidirectional channel in registry
signal_id_t sig_to_kaos(exec_env_t exec_env, int32_t signal_type, signal_id_t signal_id, uint64_t data) {    
    module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    module_registry_t *registry = get_registry_by_inst(module_inst);
    if (!registry) {
        ESP_LOGW(TAG, "%s: Module registry not found", __FUNCTION__);
        return -1;
    }

    // Signal 0 is generic liveness signal
    if (!signal_type) {
        // TODO: Error check submit_event
        submit_event(EVENT_CONTAINER_LIVE, (uint8_t *) registry, 0);
        return KaosSuccess;
    }

    kaos_signal_msg_t *event_buffer = calloc(1, sizeof(kaos_signal_msg_t));
    // Is signal id necessary
    *event_buffer = (kaos_signal_msg_t) {
        .signal_type = (kaos_signal_type_t) signal_type,
        .signal_id = (signal_id_t) signal_id,
    };

    memcpy(&(event_buffer->data), &data, sizeof(uint64_t));
    submit_event(EVENT_SIGNAL_RECEIVED, event_buffer, sizeof(kaos_signal_msg_t));

    //  esp_err_t err = start_timer(timer_handle, KAOS_SUSPEND_TIMEOUT, 0);
    // if (esp_err != ESP_OK) {
    //     ESP_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
    //     return KaosTimerError;
    // }

    return KaosSuccess;
}

// void pack_signal_data(module_inst_t module_inst, kaos_signal_msg_t signal_msg) {

// }

// TODO: Here destroy the signal timer 
// Uint32_t should be sufficient due to addressing (re: WAMR API call)
int64_t sig_from_kaos(exec_env_t exec_env, int32_t blocking) {
    // Load container
    
    module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    module_registry_t *registry = get_registry_by_inst(module_inst);
    if (!registry) {
        ESP_LOGW(TAG, "%s: Module registry not found", __FUNCTION__);
        return 0;
    }
    kaos_signal_msg_t signal_msg;
    //blocking call on signal queue 
    if (blocking) {
        if (!receive_from_queue(get_signal_queue_to_container(registry), &signal_msg, portMAX_DELAY)) {
            ESP_LOGI(TAG, "%s: Signal queue for module %s empty", __FUNCTION__, get_module_name(registry));
            return 0;
        }
    } else if (!receive_from_queue(get_signal_queue_to_container(registry), &signal_msg, TIMEOUT_QUEUE / portTICK_PERIOD_MS)) {
        ESP_LOGI(TAG, "%s: Signal queue for module %s empty", __FUNCTION__, get_module_name(registry));
        return 0;
    }

    // return wasm_runtime_module_dup_data(module_inst, (char *) &(signal_msg.signal_type), sizeof(kaos_signal_type_t));
    // uint64_t sig_type = (uint64_t) signal_msg.signal_type;
    return (int64_t) signal_msg.signal_type;
}
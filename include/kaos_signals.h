#ifndef KAOS_SIGNALS_H
#define KAOS_SIGNALS_H

#include <inttypes.h>
#include "kaos_types_shared.h"
#include "container_mgr.h"

#define KAOS_SIG_QUEUE_SIZE 3


struct module_registry_t;


// Leave LSB for response flag
typedef enum kaos_signal_type_t {
    KAOS_SIGNAL = 0,                
    KAOS_SIGNAL_RESPONSE = 1,
   
    KAOS_DATA_AVAILABLE = 2,
    KAOS_CHANNEL_AVAILABLE = 4,
    
    KAOS_SIGNAL_TERMINATE = 6,
    KAOS_SIGNAL_SANITIZE = 8,
    KAOS_SIGNAL_NEW_INPUT = 10,
    KAOS_SIGNAL_NEW_OUTPUT = 12,
    KAOS_SIGNAL_REMOVE_INPUT = 14,
    KAOS_SIGNAL_REMOVE_OUTPUT = 16,
    KAOS_SIGNAL_NEW_SENSOR = 18,
    KAOS_SIGNAL_NEW_ACTUATOR = 20,
    KAOS_SIGNAL_ISR = 22,
} kaos_signal_type_t;


typedef  uint32_t signal_id_t;

// TODO: change data to static buffer to simplify memory management
typedef struct kaos_signal_msg_t {
    kaos_signal_type_t signal_type;
    signal_id_t signal_id;
    uint64_t data;
} kaos_signal_msg_t;


bool is_response(kaos_signal_type_t signal);
queue_t init_sig_queue_to_container(void);
void destroy_sig_queues(struct module_registry_t *registry);

// TODO: Make this static
kaos_error_t release_signal_id(signal_id_t signal_id);

kaos_error_t sig_to_container(struct module_registry_t *registry, kaos_signal_msg_t* signal_msg);
int32_t sig_response_to_kaos(exec_env_t exec_env, int32_t signal_type, signal_id_t signal_id,  uint64_t data);
signal_id_t sig_to_kaos(exec_env_t exec_env, int32_t signal_type, signal_id_t signal_id,  uint64_t data);
int64_t sig_from_kaos(exec_env_t exec_env, int32_t blocking);
void handle_signal(kaos_signal_msg_t *signal_msg);

#endif
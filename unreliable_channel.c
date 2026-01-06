#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "port_logging.h"

#include "network.h"
#include "container_mgr.h"
#include "kaos_errors.h"
#include "kaos_types_shared.h"
#include "unreliable_channel.h"


#define KAOS_MAX_CONTAINER_QUEUE_ITEMS CONFIG_KAOS_MAX_CONTAINER_QUEUE_ITEMS

#define SERVICE_ID_OFFSET 0
#define SRC_ADDR_OFFSET (SERVICE_ID_OFFSET + sizeof(service_id_t))
#define TYPE_OFFSET (SRC_ADDR_OFFSET + sizeof(address_t))
#define DATA_SZ_OFFSET (TYPE_OFFSET + sizeof(char[TYPE_SZ]))
#define DATA_OFFSET (DATA_SZ_OFFSET + sizeof(data_size_t))

#define RMT_SERVICE_ID_OFFSET (0)
#define RMT_SRC_ADDR_OFFSET (RMT_SERVICE_ID_OFFSET + sizeof(service_id_t))
#define RMT_DEST_ADDR_OFFSET (RMT_SRC_ADDR_OFFSET + sizeof(address_t))
#define RMT_TYPE_OFFSET (RMT_DEST_ADDR_OFFSET + sizeof(address_t))
#define RMT_DATA_SZ_OFFSET (RMT_TYPE_OFFSET + sizeof(char[TYPE_SZ]))
#define RMT_DATA_OFFSET (RMT_DATA_SZ_OFFSET + sizeof(data_size_t))

#define MAX_QUEUE_SIZE_B (KAOS_MAX_CONTAINER_QUEUE_ITEMS * sizeof(message_t))


// #if CONFIG_KAOS_MEASUREMENT_EXEC_LATENCY
    
// #endif

static const char *TAG = "unreliable_queue";

static kaos_error_t empty_queue(queue_t *queue) {
    message_t message_dest;
    while(receive_from_queue(queue, &message_dest, 0)) {
        free(message_dest.content);
    }

    return KaosSuccess;
}


kaos_error_t destroy_channel_queue(queue_t *queue) {
    if (!queue) {
        return KaosQueueNotEstablished;
    }

    empty_queue(queue);
    delete_queue(queue);
    return KaosSuccess;
}


kaos_error_t create_channel_queue(queue_t **queue) {
    *queue = create_queue(KAOS_MAX_CONTAINER_QUEUE_ITEMS, sizeof(message_t));
    if (!(*queue)) {
        KAOS_LOGE(TAG, "Queue not created");
        return KaosQueueNotEstablished;
    }

    return KaosSuccess;
}


static kaos_error_t pack(uint8_t **dest, uint32_t *sz, message_t msg) {
    *sz = sizeof(data_size_t) + msg.content_len;
    *dest = calloc(1, *sz);

    if (!*dest) {
        return KaosMemoryAllocationError;
    }

    memcpy(*dest, &(msg.content_len), sizeof(data_size_t));
    memcpy(*dest + sizeof(data_size_t), msg.content, msg.content_len);

    return KaosSuccess;
}

// Use macros IN_QUEUE and OUT_QUEUE for mem_offset
static kaos_error_t _get(queue_t *queue, message_t *message_dest) {

    if (!queue) {
        KAOS_LOGW(__FUNCTION__, "%s->%s:%d Queue has not been established", __FILE__, __FUNCTION__, __LINE__);
        return KaosQueueNotEstablished;
    }

    // KAOS_LOGE(__FUNCTION__, "Queue get");
    if (!receive_from_queue(queue, message_dest, 0)) {
        // TODO: Uncomment
        // KAOS_LOGI(TAG, "%s: Queue %p empty", __FUNCTION__, queue);
        return KaosQueueEmptyError;
    }

    return KaosSuccess;
}

// TODO: follow registry references and synchronize holding them (don't delete reference unless it's referenced 0 times)
// Keep synchronized list of references and set them all to 0?
uint32_t get(exec_env_t exec_env, service_id_t service_id, address_t container_address, char *type) {
    KAOS_LOGI(__FUNCTION__, "Queue get from %"PRIu8":%"PRIu32" %s", service_id, container_address, type);
    message_t message;
    uint32_t buffer_size = 0;
    uint8_t *buffer = NULL;

    module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    module_registry_t *registry = get_registry_by_inst(module_inst);

    // TODO: RACE -> when container unexpectedly fails??
    pthread_mutex_lock(&registry->mutex);
    queue_t *queue = get_input_queue(registry, service_id, container_address, type);
    kaos_error_t err = _get(queue, &message);
    pthread_mutex_unlock(&registry->mutex);
    if (err) return 0;

    err = pack(&buffer, &buffer_size, message);
    if (err) {
        KAOS_LOGE(__FUNCTION__, "Kaos error: %d, expected buffer size %"PRIu32"", err, buffer_size);
        return 0;
    }

    // TODO: Why double free?
    free(message.content);
    uint32_t wasm_p = wasm_runtime_module_dup_data(module_inst, (char *) buffer, buffer_size);
    if (!wasm_p) KAOS_LOGE(__FUNCTION__, "Buffer allocation of %"PRIu32" bytes failed", buffer_size);
    free(buffer);

    // KAOS_LOGI(__FUNCTION__, "Queue get from %"PRIu8":%"PRIu32" %s", service_id, container_address, type);

    return wasm_p;
}


static kaos_error_t _put(queue_t *queue, message_t *message) {
    // KAOS_LOGI(__FUNCTION__, "Send to %d", message.dest_address);
    kaos_error_t err = KaosSuccess;
    if (!queue) {
        KAOS_LOGI(__FUNCTION__,  "%s->%d Queue flow has not been established", __FILE__, __LINE__);
        return KaosQueueNotEstablished;
    }

    if (!send_to_queue(queue, message, 0)) {
        KAOS_LOGW(TAG, "%s: Queue %p full", __FUNCTION__, queue);
        return KaosQueueFullError;
    }

    return err;
}


int32_t put(
    exec_env_t exec_env,
    service_id_t service_id, address_t src_addr, address_t dest_addr, char *type,
    uint8_t *content, data_size_t content_size) {

    // If remote put int dest queue, otherwise put into output queue
    kaos_error_t err = KaosSuccess;
    module_registry_t *src_registry = get_registry_by_inst(wasm_runtime_get_module_inst(exec_env));
    if (!src_registry) {
        KAOS_LOGE(__FUNCTION__, "%s:%d Registry with identity %"PRIu8":%"PRIu32" not found", __FILE__, __LINE__, service_id, src_addr);
        return 1;
    }

    queue_t *queue;
    pthread_mutex_lock(&src_registry->mutex);
    module_registry_t *dest_registry = get_registry_by_identity(service_id, dest_addr);
    module_registry_t *sig_registry = NULL;
    // TODO: Set new queue handle in channel entry
    if (!dest_registry) {
        queue = get_output_queue(src_registry, service_id, dest_addr, type);
        // KAOS_LOGI(__FUNCTION__, "Put in output queue %"PRIu8":%"PRIu32":%s", service_id, dest_addr, type);
    }
    else {
        pthread_mutex_lock(&dest_registry->mutex);
        queue = get_input_queue(dest_registry, service_id, src_addr, type);
        sig_registry = dest_registry;
        // KAOS_LOGI(__FUNCTION__, "Put in input queue %"PRIu8":%"PRIu32":%s", service_id, dest_addr, type);
    }

    if (!queue) {
        err = create_channel_queue(&queue);
        if (err) {
            KAOS_LOGW(__FUNCTION__, "%s->%d Queue could not be established", __FILE__, __LINE__);
            return err;
        }

        if (!dest_registry) {
            KAOS_LOGD(__FUNCTION__, "Setting new output queue");
            err = set_output_queue(src_registry, service_id, dest_addr, type, queue);
        } else {
            KAOS_LOGD(__FUNCTION__, "Setting new input queue");
            err = set_input_queue(dest_registry, service_id, src_addr, type, queue);
        }
    }

    if (err) {
        KAOS_LOGW(__FUNCTION__, "Queue not set: %d", err);
        return err;
    }

    uint8_t *msg_content = calloc(1, content_size);
    if (!msg_content) {
        pthread_mutex_unlock(&src_registry->mutex);
        if (dest_registry) pthread_mutex_unlock(&dest_registry->mutex);
        return KaosMemoryAllocationError;
    }

    memcpy(msg_content, content, content_size);
    message_t message = {
        // .service_id = service_id,
        .src_address = src_addr,
        .dest_address = dest_addr,
        .content_len = content_size,
        .content = msg_content,
    };

    // memcpy(message.type, type, TYPE_SZ);
    KAOS_LOGI(__FUNCTION__, "Putting message from %d,%d to queue", service_id, src_addr);
    err = _put(queue, &message);

    if (err) {
        KAOS_LOGW(__FUNCTION__, "Put to queue failed: %d", err);
        free(msg_content);
        pthread_mutex_unlock(&src_registry->mutex);
        if (dest_registry) pthread_mutex_unlock(&dest_registry->mutex);
        return err;
    }
    notify_and_signal(sig_registry, queue);
    // TODO: Fix this
    

    pthread_mutex_unlock(&src_registry->mutex);
    if (dest_registry) pthread_mutex_unlock(&dest_registry->mutex);

    return err;
}

static kaos_error_t parse_container_message(uint8_t *buffer, message_t *dest, service_id_t *service_id, char *type) {
    memset(dest, 0, sizeof(message_t));

    memcpy(service_id, buffer + SERVICE_ID_OFFSET, sizeof(service_id_t));
    memcpy(&dest->src_address, buffer + RMT_SRC_ADDR_OFFSET, sizeof(address_t));
    memcpy(&dest->dest_address, buffer + RMT_DEST_ADDR_OFFSET, sizeof(address_t));
    memcpy(type, buffer + RMT_TYPE_OFFSET, TYPE_SZ);
    memcpy(&dest->content_len, buffer + RMT_DATA_SZ_OFFSET, sizeof(data_size_t));

    KAOS_LOGI(TAG, "Rcvd payload sz %"PRIu32"", (uint32_t) RMT_DATA_OFFSET + dest->content_len);
    // ESP_LOG_BUFFER_HEX(TAG, buffer, (uint32_t) RMT_DATA_OFFSET + dest->content_len);

    dest->content = calloc(1, dest->content_len);
    if (!dest->content) {
        KAOS_LOGW(__FUNCTION__, "Content allocation of %"PRIu32" failed", dest->content_len);
        return KaosMemoryAllocationError;
    }
    memcpy(dest->content, buffer + RMT_DATA_OFFSET, dest->content_len);

    return KaosSuccess;
}


// TODO: remove src and dest from message buffers where not needed
kaos_error_t receive_container_payload(uint8_t *buffer) {
    kaos_error_t err;
    service_id_t service_id;
    char type[TYPE_SZ];
    message_t msg;
    parse_container_message(buffer, &msg, &service_id, type);

#if CONFIG_KAOS_MEASUREMENT_EXEC_LATENCY

#endif

    // TODO: !!! Add type
    module_registry_t *registry = get_registry_by_identity(service_id, msg.dest_address);
    if (!registry) {
        KAOS_LOGW(__FUNCTION__, "No registry %"PRIu8":%"PRIu32" ", service_id, msg.dest_address);
        return KaosRegistryNotFoundError;
    }

    queue_t *queue = get_input_queue(registry, service_id, msg.src_address, type);
    if (!queue) {
        KAOS_LOGI(__FUNCTION__, "Creating new queue for %"PRIu8":%"PRIu32"", service_id, msg.src_address);
        err = create_channel_queue(&queue);
        if (err) {
            KAOS_LOGE(__FUNCTION__, "Queue for %"PRIu8":%"PRIu32" not created", service_id, msg.src_address);
            return err;
        }

        err = set_input_queue(registry, service_id, msg.src_address, type, queue);
        if (err) {
            KAOS_LOGE(__FUNCTION__, "Queue for %"PRIu8":%"PRIu32" not created: %d", service_id, msg.src_address, err);
            return err;
        }
    }
    
    KAOS_LOGI(__FUNCTION__, "Putting message from %d,%d to queue", service_id, msg.src_address);
    KAOS_LOGI(__FUNCTION__, "Data sz %d, data %d", msg.content_len, msg);
    err = _put(queue, &msg);

    uint32_t msgs_available;
    messages_available(queue, &msgs_available);
    // TODO: Why exactly one?
    if (msgs_available == 1) {
        kaos_signal_msg_t signal_msg = {
            .signal_type = KAOS_DATA_AVAILABLE,
            .signal_id = 0,
        };

        sig_to_container(registry, &signal_msg);
    }

    return err;
}

void compose_container_message(message_t *msg, service_id_t *service_id, char *type) {
    uint16_t payload_sz = RMT_DATA_OFFSET + msg->content_len;
    uint8_t *payload_buffer = calloc(1, payload_sz);

    memcpy(payload_buffer + RMT_SERVICE_ID_OFFSET, service_id, sizeof(service_id_t));
    memcpy(payload_buffer + RMT_SRC_ADDR_OFFSET, &(msg->src_address), sizeof(address_t));
    memcpy(payload_buffer + RMT_DEST_ADDR_OFFSET, &(msg->dest_address), sizeof(address_t));
    memcpy(payload_buffer + RMT_TYPE_OFFSET, type, TYPE_SZ);
    memcpy(payload_buffer + RMT_DATA_SZ_OFFSET, &(msg->content_len), sizeof(data_size_t));

    memcpy(payload_buffer + RMT_DATA_OFFSET, msg->content, msg->content_len);

    free(msg->content);
    msg->content = payload_buffer;
    msg->content_len = payload_sz;
    // ESP_LOG_BUFFER_HEX(TAG, payload_buffer, payload_sz);
}


static module_registry_t *registry = NULL;
static int curr_registry_i = 0;
static int16_t n_channels = 0;
static int16_t curr_channel = 0;
static channel_t *channels = NULL;
static pthread_mutex_t payload_mutex = PTHREAD_MUTEX_INITIALIZER;


void validate_state(module_registry_t *to_be_destroyed) {
    pthread_mutex_lock(&payload_mutex);
    if (registry == to_be_destroyed) {
        registry = NULL;
        n_channels = 0;
        channels = NULL;
    }
    pthread_mutex_unlock(&payload_mutex);
}

// TODO: Race when adding/removing inputs from registries?
static kaos_error_t get_next_payload_from_queue(message_t *msg, service_id_t *service_id) {
    for (int i = curr_channel; i < n_channels; i++) {
        queue_t *queue = channels[i].queue;
        if (_get(queue, msg) == KaosSuccess) {
            compose_container_message(msg, &(channels[i].service_id), channels[i].type);
            *service_id = channels[i].service_id;
            curr_channel += i;

            return KaosSuccess;
        }
    }

    return KaosNothingToSend;
}

kaos_error_t get_next_container_payload(message_t *msg, service_id_t *service_id) {
    KAOS_LOGI(__FUNCTION__, "Get next payload");
    module_registry_t *registry_pool = get_registry_pool();
    pthread_mutex_lock(&payload_mutex);

    KAOS_LOGI(__FUNCTION__, "Current registry index: %d", curr_registry_i);
    for (curr_registry_i = curr_registry_i; curr_registry_i < KAOS_MAX_MODULE_N; curr_registry_i++) {
        module_registry_t *registry = registry_pool + curr_registry_i;
    
        pthread_mutex_lock(&registry->mutex);

        if (!channels) {
            channels = get_interface_config(registry).outputs;
            n_channels = get_interface_config(registry).n_outputs;
            curr_channel = 0;
        }

        if (get_next_payload_from_queue(msg, service_id) == KaosSuccess) {
            pthread_mutex_unlock(&registry->mutex);
            pthread_mutex_unlock(&payload_mutex);
            return KaosSuccess;
        }
        
        pthread_mutex_unlock(&registry->mutex);
        channels = NULL;

        if (curr_registry_i == KAOS_MAX_MODULE_N - 1) curr_registry_i = 0;
    }

    pthread_mutex_unlock(&payload_mutex);
    curr_registry_i = 0;
    
    return KaosNothingToSend;
}

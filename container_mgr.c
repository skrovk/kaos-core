#include <string.h>

#include "port_logging.h"
#include "esp_heap_caps.h"

#include "kaos_types_shared.h"
#include "unreliable_channel.h"
#include "wasm_export.h"
#include "bh_platform.h"

#include "kaos_errors.h"
#include "kaos_monitor.h"
#include "container_mgr.h"


#define KAOS_SETUP_STACK_SIZE CONFIG_KAOS_SETUP_STACK_SIZE
#define KAOS_MAX_MODULE_N CONFIG_KAOS_MAX_MODULE_N

static const char *MGR_TAG = "container_mgr";
// static module_registry_t *module_registry_tail = NULL;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static module_registry_t registry_pool[KAOS_MAX_MODULE_N];


void init_registry_pool(void) {
    memset(registry_pool, 0, sizeof(registry_pool));
}

module_registry_t *get_registry_pool() {
    return registry_pool;
}


kaos_error_t destroy_channel(channel_t channel) {
    return destroy_channel_queue(channel.queue);
}


void print_channel(channel_t channel) {
    KAOS_LOGI(__FUNCTION__, "    %"PRIu8":%"PRIu32" %s", channel.service_id, channel.address, channel.type);
}


// TODO: Check channels for existence of 1:1 input
channel_t *fetch_channel(channel_t *channels, int16_t n_channels, service_id_t service_id, address_t address, char *type) {
    for (int i = 0; i < n_channels; i++) {
        channel_t channel = channels[i];
        if (
            (channel.service_id == service_id) &&
            (channel.address == address) &&
            (!memcmp(channel.type, type, TYPE_SZ))
           ) {
            return channels + i;
        }
    }

    return NULL;
}


static void destroy_exec_env(exec_env_t exec_env) {
    wasm_runtime_destroy_exec_env(exec_env);
}


static void destroy_runtime() {
    wasm_runtime_destroy();
}


static void destroy_module_instance(module_inst_t module_inst) {
    wasm_runtime_deinstantiate(module_inst);
}


static void unload_module(module_t module_handle) {
    wasm_runtime_unload(module_handle);
}


static void set_module_handle(module_registry_t *registry, module_t module_handle) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return;
    }
    
    pthread_mutex_lock(&registry->mutex);
    registry->module_handle = module_handle;
    pthread_mutex_unlock(&registry->mutex);

}

static module_t get_module_handle(module_registry_t *registry) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return NULL;
    }
    
    pthread_mutex_lock(&registry->mutex);
    module_t handle = registry->module_handle;
    pthread_mutex_unlock(&registry->mutex);

    return handle;
}


// static void set_registry_prev(module_registry_t *registry, module_registry_t *prev) {
//     if (!registry) {
//         KAOS_LOGE(__FUNCTION__, "Registry is null");
//         return;
//     }
    
//     pthread_mutex_lock(&registry->mutex);
//     registry->prev = prev;
//     pthread_mutex_unlock(&registry->mutex);
// }


// module_registry_t *get_registry_prev(module_registry_t *registry) {
//     if (!registry) {
//         KAOS_LOGE(__FUNCTION__, "Registry is null");
//         return NULL;
//     }
    
//     pthread_mutex_lock(&registry->mutex);

//     module_registry_t *prev = registry->prev;
//     pthread_mutex_unlock(&registry->mutex);

//     return prev;
// }


static void set_module_name(module_registry_t *registry, char *module_name) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return;
    }
    
    pthread_mutex_lock(&registry->mutex);

    memset(registry->module_name, 0, sizeof(registry->module_name));
    size_t name_len = ((strlen((char *) module_name) + 1) > sizeof(registry->module_name)) ? sizeof(registry->module_name) : strlen((char *) module_name);
    memcpy((char *) registry->module_name, module_name, name_len);

    pthread_mutex_unlock(&registry->mutex);
}


char *get_module_name(module_registry_t *registry) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return NULL;
    }
    
    pthread_mutex_lock(&registry->mutex);

    char *name = (char *) registry->module_name;
    pthread_mutex_unlock(&registry->mutex);

    return name;
}


static void set_module_inst(module_registry_t *registry, module_inst_t module_inst) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return;
    }
    
    pthread_mutex_lock(&(registry->mutex));
    registry->module_inst = module_inst;
    pthread_mutex_unlock(&registry->mutex);
}


module_inst_t get_container_inst(module_registry_t *registry) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return NULL;
    }
    
    if (pthread_mutex_lock(&(registry->mutex))) {
        KAOS_LOGE(__FUNCTION__, "Lock error");
        return NULL;
    }
    
    module_inst_t inst = registry->module_inst;
    pthread_mutex_unlock(&registry->mutex);

    return inst;
}


static void set_exec_env(module_registry_t *registry, exec_env_t exec_env) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return;
    }
    
    pthread_mutex_lock(&registry->mutex);

    registry->exec_env = exec_env;
    pthread_mutex_unlock(&registry->mutex);
}


static exec_env_t get_exec_env(module_registry_t *registry) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return NULL;
    }
    
    pthread_mutex_lock(&registry->mutex);

    exec_env_t exec_env = registry->exec_env;
    pthread_mutex_unlock(&registry->mutex);

    return exec_env;
}


void set_signal_queue(module_registry_t *registry, queue_t *to_container) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return;
    }
    
    pthread_mutex_lock(&registry->mutex);
    registry->signal_queue_to_container = to_container;
    pthread_mutex_unlock(&registry->mutex);
}


queue_t *get_signal_queue_to_container(module_registry_t *registry) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return NULL;
    }
    
    pthread_mutex_lock(&registry->mutex);
    queue_t *queue = registry->signal_queue_to_container;
    pthread_mutex_unlock(&registry->mutex);
    return queue;
}


void set_status(module_registry_t *registry, status_t status) {
    pthread_mutex_lock(&registry->mutex);

    registry->status = status;
    pthread_mutex_unlock(&registry->mutex);
}


status_t get_status(module_registry_t *registry) {
    pthread_mutex_lock(&registry->mutex);
    status_t status = registry->status;
    pthread_mutex_unlock(&registry->mutex);

    return status;
}


void set_thread_id(module_registry_t *registry, pthread_t thread_id) {
    pthread_mutex_lock(&registry->mutex);

    registry->thread_id = thread_id;
    pthread_mutex_unlock(&registry->mutex);
}


pthread_t get_thread_id(module_registry_t *registry) {
    pthread_mutex_lock(&registry->mutex);

    pthread_t thread_id = registry->thread_id;
    pthread_mutex_unlock(&registry->mutex);

    return thread_id;
}


static kaos_error_t set_module_config(module_registry_t *registry, module_config_t *module_config) {
    pthread_mutex_lock(&registry->mutex);

    registry->module_config.heap_size = module_config->heap_size;
    registry->module_config.stack_size = module_config->stack_size;
    
    if (!module_config->source_buffer_size) {
        pthread_mutex_unlock(&registry->mutex);
        return KaosModuleConfigError;
    }
    registry->module_config.source_buffer_size = module_config->source_buffer_size;

    registry->module_config.source_buffer = calloc(1, module_config->source_buffer_size);
    if (!registry->module_config.source_buffer) {
        pthread_mutex_unlock(&registry->mutex);
        return KaosMemoryAllocationError;
    }

    memcpy(registry->module_config.source_buffer, module_config->source_buffer, module_config->source_buffer_size);

    pthread_mutex_unlock(&registry->mutex);
    return KaosSuccess;
}


module_config_t get_module_config(module_registry_t *registry) {
    pthread_mutex_lock(&registry->mutex);

    module_config_t module_config = registry->module_config;
    pthread_mutex_unlock(&registry->mutex);

    return module_config;
}


static void free_module_config(module_registry_t *registry) {
    pthread_mutex_lock(&registry->mutex);


    if (registry->module_config.source_buffer) free(registry->module_config.source_buffer);
    registry->module_config.source_buffer = NULL;
    registry->module_config.source_buffer_size = 0;

    pthread_mutex_unlock(&registry->mutex);
}


static kaos_error_t update_module_config(module_registry_t *registry, module_config_t *new_module_config) {
    pthread_mutex_lock(&registry->mutex);

    if (new_module_config->heap_size) {
        registry->module_config.heap_size = new_module_config->heap_size;
    }

    if (new_module_config->stack_size) {
        registry->module_config.stack_size = new_module_config->stack_size;
    }

    if (new_module_config->source_buffer_size) {
        KAOS_LOGE(__FUNCTION__, "buffer size %"PRIu32"", new_module_config->source_buffer_size);
        if (registry->module_config.source_buffer) free(registry->module_config.source_buffer);
        registry->module_config.source_buffer_size = new_module_config->source_buffer_size;

        if (registry->module_config.source_buffer_size == new_module_config->source_buffer_size) {
            KAOS_LOGW(__FUNCTION__, "Check container buffer freshness - runtime cannot operate on previously used wasm source buffer");
        }
        registry->module_config.source_buffer = calloc(1, new_module_config->source_buffer_size);
        if (!registry->module_config.source_buffer) {
            pthread_mutex_unlock(&registry->mutex);
            return KaosMemoryAllocationError;
        }

        memcpy(registry->module_config.source_buffer, new_module_config->source_buffer, new_module_config->source_buffer_size);
    }

    pthread_mutex_unlock(&registry->mutex);

    return KaosSuccess;
}


// Local helper to duplicate channel arrays for interface configuration fields.
static kaos_error_t alloc_channel_field(channel_t **dest_field, channel_t *src_field, int16_t n_channels) {
     *dest_field = calloc(n_channels, sizeof(channel_t));
     if (!*dest_field) {
         KAOS_LOGE(__FUNCTION__, "Resouce %d memory allocation failed", n_channels);
         return KaosMemoryAllocationError;
     }
     memcpy(*dest_field, src_field, n_channels * sizeof(channel_t));
 
     return KaosSuccess;
 }
 
 
 static kaos_error_t set_interface_config(module_registry_t *registry, interface_config_t *config) {
    pthread_mutex_lock(&registry->mutex);
    kaos_error_t err = KaosSuccess;
 
    // All these have to be defined in a valid module interface configuration, although empty is allowed
    if ((config->n_identities < 1) || (config->n_inputs < 0) || (config->n_outputs < 0) || (config->n_resources < 0)) {
        pthread_mutex_unlock(&registry->mutex);
        return KaosModuleConfigError;
    }
 
    channel_t *identities = NULL;
    channel_t *inputs = NULL;
    channel_t *outputs = NULL;
    export_symbol_t *resources = NULL;

    err = alloc_channel_field(&identities, config->identities, config->n_identities);
    if (err) goto cleanup;

    if (config->n_inputs) {
        err = alloc_channel_field(&inputs, config->inputs, config->n_inputs);
        if (err) goto cleanup;
    }
 
    if (config->n_outputs) {
        err = alloc_channel_field(&outputs, config->outputs, config->n_outputs);
        if (err) goto cleanup;
    }
 
    if (config->n_resources) {
        KAOS_LOGI(__FUNCTION__, "Allocating %d resources", config->n_resources);
        resources = calloc(config->n_resources, sizeof(export_symbol_t));
        if (!resources) {
            KAOS_LOGE(__FUNCTION__, "Resouce memory allocation failed");
            goto cleanup;
        }
        memcpy(resources, config->resources, config->n_resources * sizeof(export_symbol_t));

    }
 
    registry->intfc_config.n_identities = config->n_identities;
    registry->intfc_config.identities = identities;
    registry->intfc_config.n_inputs = config->n_inputs;
    registry->intfc_config.inputs = inputs;
    registry->intfc_config.n_outputs = config->n_outputs;
    registry->intfc_config.outputs = outputs;
    registry->intfc_config.n_resources = config->n_resources;
    registry->intfc_config.resources = resources;
 
cleanup:
    if (err) {
        KAOS_LOGE(__FUNCTION__, "Resouce memory allocation failed");
        err = KaosMemoryAllocationError;
        free(identities);
        free(inputs);
        free(outputs);
        free(resources);
    }
 
     pthread_mutex_unlock(&registry->mutex);
    return err;
}
 

interface_config_t get_interface_config(module_registry_t *registry) {
    pthread_mutex_lock(&registry->mutex);

    interface_config_t interface_config = registry->intfc_config;
    pthread_mutex_unlock(&registry->mutex);

    return interface_config;
}

// // This is wrong 
// int assign_timer_id(module_registry_t *registry) {
//     int id = -1;
//     pthread_mutex_lock(&registry->mutex);
//     for (int i = 0; i < KAOS_MODULE_TIMERS_N; i++) {
//         if (registry->timers[i] == NULL) {
//             id = i;
//             break;
//         }
//     }
//     pthread_mutex_unlock(&registry->mutex);
//     return id;
// }

// kaos_error_t release_timer_id(module_registry_t *registry, int timer_id) {
//     pthread_mutex_lock(&registry->mutex);
//     if (registry->timers[timer_id] == NULL) {
//         pthread_mutex_unlock(&registry->mutex);
//         return KaosContainerTimerNotFoundError;
//     }
//     pthread_mutex_unlock(&registry->mutex);
//     return KaosSuccess;
// }


// Timers are only accessed from monitor loop - access doesn't need to be synchronized
kaos_timer_handle_t *get_timer(module_registry_t *registry, int timer_id) {    
    kaos_timer_handle_t *timer = NULL;
    
    pthread_mutex_lock(&registry->mutex);
    timer = registry->timers[timer_id];
    pthread_mutex_unlock(&registry->mutex);

    return timer;
}


// Timers are only accessed from monitor loop - access doesn't need to be synchronized
void set_timer(module_registry_t *registry, kaos_timer_handle_t *handle, int timer_id) {
    pthread_mutex_lock(&registry->mutex);
    registry->timers[timer_id] = handle;
    pthread_mutex_unlock(&registry->mutex);

    return;
}


kaos_error_t set_input_queue(module_registry_t *registry, service_id_t service_id, address_t address, char *type, queue_t *queue) {
    pthread_mutex_lock(&registry->mutex);
    channel_t *target_channel = fetch_channel(registry->intfc_config.inputs, registry->intfc_config.n_inputs, service_id, address, type);
    if (!target_channel) {
        pthread_mutex_unlock(&registry->mutex);
        return KaosChannelNotFoundError;
    }

    target_channel->queue = queue;
    pthread_mutex_unlock(&registry->mutex);

    return KaosSuccess;
}


kaos_error_t set_output_queue(module_registry_t *registry, service_id_t service_id, address_t address, char *type, queue_t *queue) {
    pthread_mutex_lock(&registry->mutex);
    channel_t *target_channel = fetch_channel(registry->intfc_config.outputs, registry->intfc_config.n_outputs, service_id, address, type);
    if (!target_channel) {
        pthread_mutex_unlock(&registry->mutex);
        return KaosChannelNotFoundError;
    }

    target_channel->queue = queue;
    pthread_mutex_unlock(&registry->mutex);

    return KaosSuccess;
}


queue_t *get_input_queue(module_registry_t *registry, service_id_t service_id, address_t address, char *type) {
    pthread_mutex_lock(&registry->mutex);
    channel_t *target_channel = fetch_channel(registry->intfc_config.inputs, registry->intfc_config.n_inputs, service_id, address, type);
    if (!target_channel) {
        pthread_mutex_unlock(&registry->mutex);
        return NULL;
    }

    queue_t *queue = target_channel->queue;
    pthread_mutex_unlock(&registry->mutex);

    return queue;
}


queue_t *get_output_queue(module_registry_t *registry, service_id_t service_id, address_t address, char *type) {
    pthread_mutex_lock(&registry->mutex);
    channel_t *target_channel = fetch_channel(registry->intfc_config.outputs, registry->intfc_config.n_outputs, service_id, address, type);
    if (!target_channel) {
        pthread_mutex_unlock(&registry->mutex);
        return NULL;
    }

    queue_t *queue = target_channel->queue;
    pthread_mutex_unlock(&registry->mutex);

    return queue;
}

static void free_channels(channel_t *channels, int16_t n_channels) {
    for (int i = 0; i < n_channels; i++) {
        destroy_channel(channels[i]);
    }
}

static void free_interface_config(module_registry_t *registry) {
    pthread_mutex_lock(&registry->mutex);
    if (registry->intfc_config.identities) {
        free_channels(registry->intfc_config.identities, registry->intfc_config.n_identities);
        free(registry->intfc_config.identities);
        registry->intfc_config.identities = NULL;
        registry->intfc_config.n_identities = 0;
    }

    if (registry->intfc_config.inputs) {
        free_channels(registry->intfc_config.inputs, registry->intfc_config.n_inputs);
        free(registry->intfc_config.inputs);
        registry->intfc_config.inputs = NULL;
        registry->intfc_config.n_inputs = 0;
    }

    if (registry->intfc_config.outputs) {
        free_channels(registry->intfc_config.outputs, registry->intfc_config.n_outputs);
        free(registry->intfc_config.outputs);
        registry->intfc_config.outputs = NULL;
        registry->intfc_config.n_outputs = 0;
    }

    if (registry->intfc_config.resources) {
        KAOS_LOGE(__FUNCTION__, "Freeing %d resources", registry->intfc_config.n_resources);
        free(registry->intfc_config.resources);
        registry->intfc_config.resources = NULL;
        registry->intfc_config.resources = 0;
    }

    pthread_mutex_unlock(&registry->mutex);
}

static kaos_error_t update_channels(channel_t **dest, channel_t *old, int16_t n_old, channel_t *new, int16_t n_new) {
    // Remove pass
    int16_t *kept = calloc(n_old, sizeof(int16_t));
    if (n_old && !kept) return KaosMemoryAllocationError;
    int16_t n_kept = 0;

    for (int i = 0; i < n_old; i++) {
        if (!fetch_channel(new, n_new, old[i].service_id, old[i].address, old[i].type)) {
            destroy_channel(old[i]);
            continue;
        }
        kept[n_kept] = i;
        n_kept++;
    }

    int16_t *added = calloc(n_new, sizeof(int16_t));
    if (n_new && !added) return KaosMemoryAllocationError;
    int16_t n_added = 0;

    for (int i = 0; i < n_new; i++) {
        if (!fetch_channel(old, n_old, new[i].service_id, new[i].address, new[i].type)) {
            added[n_added] = i;
            n_added++;
        }
    }

    int16_t new_set_sz = n_kept + n_added;
    channel_t *new_set = calloc(new_set_sz, sizeof(channel_t));
    if ( (new_set_sz) && !new_set) return KaosMemoryAllocationError;

    for (int i = 0; i < n_kept; i++) {
        new_set[i] = old[kept[i]];
    }

    for (int i = n_kept; i < new_set_sz; i++) {
        new_set[i] = new[added[i]];
    }

    if (new_set_sz != n_new) return KaosModuleConfigError;

    free(kept);
    free(added);
    *dest = new_set;
    return KaosSuccess;
}


static kaos_error_t update_interface_config(module_registry_t *registry, interface_config_t *new_config) {
    pthread_mutex_lock(&registry->mutex);

    kaos_error_t err;
    if (new_config->n_identities != (int16_t) -1) {
        // KAOS_LOGI(__FUNCTION__, "Update identities");
        channel_t *updated;
        err = update_channels(
            &updated,
            registry->intfc_config.identities,
            registry->intfc_config.n_identities,
            new_config->identities, new_config->n_identities
        );
        if (err) return err;

        if (registry->intfc_config.identities) free(registry->intfc_config.identities);
        registry->intfc_config.identities = updated;
        registry->intfc_config.n_identities = new_config->n_identities;
    }
// TODO: REFACTOR
    if (new_config->n_inputs != (int16_t) -1) {
        // KAOS_LOGI(__FUNCTION__, "Update inputs");
        channel_t *updated;
        err = update_channels(
            &updated,
            registry->intfc_config.inputs,
            registry->intfc_config.n_inputs,
            new_config->inputs, new_config->n_inputs
        );
        if (err) return err;

        if (registry->intfc_config.inputs) free(registry->intfc_config.inputs);
        registry->intfc_config.inputs = updated;
        registry->intfc_config.n_inputs = new_config->n_inputs;
    }

    if (new_config->n_outputs != (int16_t) -1) {
        // KAOS_LOGI(__FUNCTION__, "Update outputs");
        channel_t *updated;
        err = update_channels(
            &updated,
            registry->intfc_config.outputs,
            registry->intfc_config.n_outputs,
            new_config->outputs, new_config->n_outputs
        );
        if (err) return err;

        if (registry->intfc_config.outputs) free(registry->intfc_config.outputs);
        registry->intfc_config.outputs = updated;
        registry->intfc_config.n_outputs = new_config->n_outputs;
    }

    if (new_config->n_resources != (int16_t) -1) {
        // KAOS_LOGI(__FUNCTION__, "Update resources");
        registry->intfc_config.n_resources = new_config->n_resources;

        if (registry->intfc_config.resources) free(registry->intfc_config.resources);
        registry->intfc_config.resources = calloc(new_config->n_resources, sizeof(export_symbol_t));
        if (!registry->intfc_config.resources) {
            pthread_mutex_unlock(&registry->mutex);
            return KaosMemoryAllocationError;
        }
        memcpy(registry->intfc_config.resources, new_config->resources, new_config->n_resources * sizeof(export_symbol_t));
    }

    pthread_mutex_unlock(&registry->mutex);
    return KaosSuccess;
}


static module_registry_t *create_new_registry(void) {
    int registry_i = -1;
    
    pthread_mutex_lock(&registry_mutex);
    for (int i = 0; i < KAOS_MAX_MODULE_N; i++) {
        if (registry_pool[i].status == EMPTY) {
            registry_pool[i].status = SUSPENDED;
            registry_i = i;
            break;
        }
    }
    pthread_mutex_unlock(&registry_mutex);

    if (registry_i < 0) {
        return NULL;
    }


    int err;
    if ((err = pthread_mutexattr_init(&registry_pool[registry_i].mutex_attr))) {
        KAOS_LOGE(__FUNCTION__, "Mutexattr not created %d", err);
        return NULL;
    }
    if ((err = pthread_mutexattr_settype(&registry_pool[registry_i].mutex_attr, PTHREAD_MUTEX_RECURSIVE))) {
        KAOS_LOGE(__FUNCTION__, "Mutexattr not set %d", err);
        return NULL;
    }
    if ((err = pthread_mutex_init(&registry_pool[registry_i].mutex, &registry_pool[registry_i].mutex_attr)))  {
        KAOS_LOGE(__FUNCTION__, "Mutex not initalized: %d", err);
        return NULL;
    }

    return &registry_pool[registry_i];
}

void cancel_timers(module_registry_t *registry) {
    for (int i = 0; i < KAOS_MODULE_TIMERS_N; i++) {
        if (!(registry->timers[i])) continue; 
        
        if (ESP_OK != stop_timer(registry->timers[i])) {
            KAOS_LOGW(__FUNCTION__, "Timer not stopped %p", registry->timers[i]);
        }
        
        if (ESP_OK != delete_timer(registry->timers[i])) {
            KAOS_LOGE(__FUNCTION__, "Timer not deleted %p", registry->timers[i]);
        } 
    }
}


static kaos_error_t destroy_registry(module_registry_t *registry) {
    if (!registry) {
        KAOS_LOGE(__FUNCTION__, "Registry is null");
        return KaosRegistryNotFoundError;
    }
    
    pthread_mutex_lock(&registry->mutex);
    cancel_timers(registry);
    validate_state(registry);

    free_module_config(registry);
    // TODO: Queue registry refactor: Deallocate queues in input/output channelse
    free_interface_config(registry);
    destroy_sig_queues(registry);

    pthread_mutex_unlock(&registry->mutex);
    pthread_mutex_destroy(&registry->mutex);
    pthread_mutexattr_destroy(&registry->mutex_attr);
    memset(registry, 0, sizeof(module_registry_t));
    registry->status = EMPTY;
    
    return KaosSuccess;
}


module_registry_t *get_registry_by_handle(module_t module_handle) {
    if (!module_handle) {
        KAOS_LOGE(__FUNCTION__, "Module handle is null");
        return NULL;
    }
    
    module_registry_t *registry = get_registry_pool();

    for (int i = 0; i < KAOS_MAX_MODULE_N; i++) {
        if ((registry + i)->status == EMPTY) continue;
        if (get_module_handle(registry + i) == module_handle) {
            return registry + i;
        }
    }

    return NULL;
}


module_registry_t *get_registry_by_name(char *module_name) {
    if (!module_name) {
        KAOS_LOGE(__FUNCTION__, "Module name is null");
        return NULL;
    }
    
    module_registry_t *registry = get_registry_pool();

    for (int i = 0; i < KAOS_MAX_MODULE_N; i++) {
        if ((registry + i)->status == EMPTY) continue;
        if (strcmp((char *) get_module_name(registry + i), (char *) module_name) == 0) {
            return registry + i;
        }
    }

    return NULL;
}


module_registry_t *get_registry_by_inst(module_inst_t module_inst) {
    if (!module_inst) {
        return NULL;
    }
    
    module_registry_t *registry = get_registry_pool();

    for (int i = 0; i < KAOS_MAX_MODULE_N; i++) {
        if ((registry + i)->status == EMPTY) continue;
        if (get_container_inst(registry + i) == module_inst) {
            return registry + i;
        }
    }

    return NULL;
}

module_registry_t *get_registry_by_identity(service_id_t service_id, address_t address) {
    module_registry_t *registry = get_registry_pool();

    for (int i = 0; i < KAOS_MAX_MODULE_N; i++) {
        if ((registry + i)->status == EMPTY) continue;
        interface_config_t intfc_conf = get_interface_config(registry + i);

        for (int j = 0; j< intfc_conf.n_identities; j++) {
            channel_t id = intfc_conf.identities[j];
            if ((service_id == id.service_id) && (address == id.address)) {
                return registry + i;
            }
        }
    }

    return NULL;
}


module_registry_t *get_registry_by_sig_handle(queue_t *handle) {
    if (!handle) {
        KAOS_LOGE(__FUNCTION__, "Signal handle is null");
        return NULL;
    }
    
    module_registry_t *registry = get_registry_pool();

    for (int i = 0; i < KAOS_MAX_MODULE_N; i++) {
        if ((registry + i)->status == EMPTY) continue;
        if (get_signal_queue_to_container(registry + i) == handle) {
            return registry + i;
        }
    }

    return NULL;
}

// Total mem size
// list registries by sz
// 
// How many registries are running
// registry_stats_t get_registry_stats() {

// } 


kaos_error_t init_runtime() {
    int result = wasm_runtime_init();
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_VERBOSE);
    if (!result) return KaosRuntimeError;

    // TODO: !Fix sz calculation
    // int wamr_err = wasm_runtime_register_natives("env", kaos_native_symbols, 13);

    // if (!wamr_err) {
    //     KAOS_LOGE(MGR_TAG, "Error: Native symbols not registered");
    //     return KaosExportRegistrationError;
    // }


    return KaosSuccess;
}

// TODO 
// Terminate sets container trap. Once trap is set, execute_function returns and container is marked as suspended. This raises the module suspended event which destroys module instance and unloads the module.
kaos_error_t suspend_module(module_registry_t *registry) {
    KAOS_LOGW(__FUNCTION__, "Suspending module %s", registry->intfc_config.identities);
    // TODO: return error here
    if (!registry) {
        return KaosSuccess;
    }

    module_inst_t inst = get_container_inst(registry);
    if (!inst) {
        KAOS_LOGE(__FUNCTION__, "Container trap could not be set - module inst not found");
        return KaosRegistryNotFoundError;
    }

    wasm_runtime_terminate(inst);
    return KaosSuccess;
}

void suspend_module_cleanup(module_registry_t *registry) {
    destroy_module_instance(get_container_inst(registry));
    unload_module(get_module_handle(registry));
}


// TODO: This should be 
void destroy_module(module_registry_t *registry) {
    // suspend_module(registry);
    destroy_registry(registry);
}


void destroy_module_registries() {
    module_registry_t *registry = get_registry_pool();

    for (int i = 0; i < KAOS_MAX_MODULE_N; i++) {
        if (registry[i].status == EMPTY) continue;
        destroy_registry(registry + i);
    }
}


static kaos_error_t load_module(module_registry_t *registry) {

    // WAMR errors are bool: 0 failure, 1 success
    int wamr_err;
    char error_buf[128];
    interface_config_t interface = get_interface_config(registry);
    module_config_t config = get_module_config(registry);

    KAOS_LOGI(MGR_TAG, "Registering %d native symbols", interface.n_resources);

    if (interface.n_resources) {
        // TODO: per module native registration
        wamr_err = wasm_runtime_register_natives("env", interface.resources, interface.n_resources);

        if (!wamr_err) {
        KAOS_LOGE(MGR_TAG, "Error: Native symbols not registered");
        return KaosExportRegistrationError;
        }
    }
// Need new src
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(MGR_TAG, "Heap integrity check failed before wasm_runtime_load");
        return KaosMemoryAllocationError;
    }

    // KAOS_LOGI(MGR_TAG, "Loading module from buffer %p of size %"PRIu32"", config.source_buffer, config.source_buffer_size);

    // TODO: Increasing buffer size to 3677, 3673, 3668, 3665,  bytes initiates seemingly randomg loading error, even though heap check passes (what sizes exactly set it off?)
//    Succeed: 3666
    // module_t module = wasm_runtime_load(test_wasm,
    //                                     sizeof(test_wasm),
    //                                     error_buf,
    //                                     sizeof(error_buf)
    //                                     );
    module_t module = wasm_runtime_load(config.source_buffer,
                                        config.source_buffer_size,
                                        error_buf,
                                        sizeof(error_buf)
                                        );
    
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(MGR_TAG, "Heap integrity check failed after wasm_runtime_load");
        return KaosMemoryAllocationError;
    }
    if (!module) {
        KAOS_LOGE(MGR_TAG, "Loading Error %p: %s\n", module, error_buf);
        return KaosModuleInitializationError;
    }

    set_module_handle(registry, module);
    // bool result = wasm_runtime_register_module(registry->module_name, module, error_buf, 128);

    // if (!result) {
    //     KAOS_LOGW(MGR_TAG, "Error: Module not registered");
    //     return KaosModuleInitializationError;
    // }

    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(MGR_TAG, "Heap integrity check failed after setting module handle");
        return KaosMemoryAllocationError;
    }

    module_inst_t module_inst = wasm_runtime_instantiate(module,
                                                         config.stack_size,
                                                         config.heap_size,
                                                         error_buf,
                                                         sizeof(error_buf));
    if (!module_inst) {
        KAOS_LOGE(MGR_TAG, "Instantiate error %p: %s\n", module_inst, error_buf);
        destroy_registry(registry);
        return KaosModuleInitializationError;
    }

    set_module_inst(registry, module_inst);

    return KaosSuccess;
}

void convert_channel_format(container_channel_t *c_channels, channel_t *channels, int16_t n_channels) {
    for (int i = 0; i < n_channels; i++) {
        c_channels[i].service_id = channels[i].service_id;
        c_channels[i].address = channels[i].address;
        memcpy(c_channels[i].type, channels[i].type, TYPE_SZ);
    }
}

// Runs after container is initialized but before the main function is executed. Passes input and output messsge channel information to the container, which saves to be able to 
// submit and retrieve application data. 
kaos_error_t run_setup(module_registry_t *registry) {
    module_inst_t inst = get_container_inst(registry);
    interface_config_t interface = get_interface_config(registry);

    container_channel_t container_identities[interface.n_identities];
    container_channel_t container_inputs[interface.n_inputs];
    container_channel_t container_outputs[interface.n_outputs];

    convert_channel_format(container_identities, interface.identities, interface.n_identities);
    convert_channel_format(container_inputs, interface.inputs, interface.n_inputs);
    convert_channel_format(container_outputs, interface.outputs, interface.n_outputs);

    //Register inputs, outputs
    uint32_t identity_p = wasm_runtime_module_dup_data(inst, (char *) container_identities, interface.n_identities * sizeof(container_channel_t));
    uint32_t input_p = wasm_runtime_module_dup_data(inst, (char *) container_inputs, interface.n_inputs * sizeof(container_channel_t));
    uint32_t output_p = wasm_runtime_module_dup_data(inst, (char *) container_outputs, interface.n_outputs * sizeof(container_channel_t));

    if (!(identity_p ^ (uint32_t) interface.n_identities) || !(input_p ^ (uint32_t) interface.n_inputs) || !(output_p^ (uint32_t) interface.n_inputs)) {
        KAOS_LOGE(__FUNCTION__, "Memory dup for setup unsuccessful");
        return KaosContainerMemoryAllocationError;
    }

    /* Verify heap integrity before handing received buffer to monitor */
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(MGR_TAG, "Heap corruption detected after interface setup");
        
        return KaosMemoryAllocationError;
    }

    uint32_t argv[] = {
        identity_p,
        interface.n_identities,
        input_p,
        interface.n_inputs,
        output_p,
        interface.n_outputs,
    };

    // TODO: Set time limit for execution
    kaos_error_t err =  execute_function(registry, (char *) SETUP_FUNC_NAME, argv, SETUP_FUNC_ARG_N, KAOS_SETUP_STACK_SIZE);
    if (err) return err;

    wasm_runtime_module_free(inst, identity_p);
    wasm_runtime_module_free(inst, input_p);
    wasm_runtime_module_free(inst, output_p);

    return argv[0];
}

// Load modules and run a setup function for the module instance
// assigning flows the container takes a part in
kaos_error_t init_module(module_registry_t **registry_dest, char *module_name, module_config_t *config, interface_config_t *interface) {
    kaos_error_t err;

    module_registry_t *module_registry = NULL;
    module_registry = create_new_registry();
    if (!module_registry) {
        KAOS_LOGE(__FUNCTION__, "New registry not created");
        return KaosModuleInitializationError;
    }

    set_module_name(module_registry, module_name);
    set_thread_id(module_registry, pthread_self());
    set_status(module_registry, SUSPENDED);

    /* Verify heap integrity before handing received buffer to monitor */
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(MGR_TAG, "Heap corruption detected after setting status");
        
        return KaosMemoryAllocationError;
    }
    
    if ((err = set_module_config(module_registry, config))) {
        KAOS_LOGE(MGR_TAG, "Module configuration error");
        destroy_registry(module_registry);
        return err;  
    } 

    /* Verify heap integrity before handing received buffer to monitor */
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(MGR_TAG, "Heap corruption detected after setting module config");
        
        return KaosMemoryAllocationError;
    }

    if ((err = set_interface_config(module_registry, interface))) {
        KAOS_LOGE(MGR_TAG, "Module interface configuration error");
        destroy_registry(module_registry);
        return err;  
    } 

    /* Verify heap integrity before handing received buffer to monitor */
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(MGR_TAG, "Heap corruption detected after setting interface config");
        
        return KaosMemoryAllocationError;
    }

    queue_t *sig_queue = init_sig_queue_to_container();
    if (!sig_queue) {
        KAOS_LOGE(MGR_TAG, "Signal queue init failed");
        destroy_registry(module_registry);
        return KaosModuleInitializationError;
    }

    set_signal_queue(module_registry, sig_queue);

    /* Verify heap integrity before handing received buffer to monitor */
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(MGR_TAG, "Heap corruption detected before loading module");
        
        return KaosMemoryAllocationError;
    }

    err = load_module(module_registry);
    if (err) {
        destroy_registry(module_registry);
        return KaosModuleInitializationError;
    }

    /* Verify heap integrity before handing received buffer to monitor */
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(MGR_TAG, "Heap corruption detected after loading module");
        
        // return KaosMemoryAllocationError;
    }

    err = run_setup(module_registry);
    if (err) {
        destroy_registry(module_registry);
        return KaosModuleInitializationError;
    }
    KAOS_LOGI(MGR_TAG, "Setup done");
    *registry_dest = module_registry;


    return KaosSuccess;
}

// TODO: replace by fetch_channel
static bool channel_exists(channel_t *channels, uint32_t channel_n, channel_t target) {
    for (int i = 0; i < channel_n; i++) {
        if ((channels[i].address == target.address) && (channels[i].service_id == target.service_id) && (!strcmp(channels[i].type, target.type))) {
            return true;
        }
    }

    return false;
}

uint32_t build_channel_set(channel_t *channel_dest, channel_t *inputs, uint32_t input_n, channel_t *outputs, uint32_t output_n) {
    uint32_t channel_set_size = 0;

    for (int i = 0; i < input_n; i++) {
        if (!channel_exists(channel_dest, channel_set_size, inputs[i])) {
            channel_dest[channel_set_size] = inputs[i];
            channel_set_size += 1;
        }
    }

    for (int i = 0; i < output_n; i++) {
        if (!channel_exists(channel_dest, channel_set_size, outputs[i])) {
            channel_dest[channel_set_size] = outputs[i];
            channel_set_size += 1;
        }
    }

    return channel_set_size;
}

// TODO: Implement support for channel label identification
// TODO: Remove module name from function args
kaos_error_t reinit_module(module_registry_t *registry, char *module_name, module_config_t *config, interface_config_t *interface) {
    kaos_error_t err;

    // KAOS_LOGI(__FUNCTION__, "Update interface config");
    print_interface(registry->intfc_config);

    err = update_interface_config(registry, interface);
    if (err) {
        destroy_registry(registry);
        return err;
    }

    print_interface(registry->intfc_config);

    // KAOS_LOGI(__FUNCTION__, "Update module config");

    print_config(registry->module_config);
    err = update_module_config(registry, config);
    if (err) {
        destroy_registry(registry);
        return err;
    }
    print_config(registry->module_config);

    set_thread_id(registry, pthread_self());

    KAOS_LOGI(__FUNCTION__, "Load");

    err = load_module(registry);
    if (err) return KaosModuleInitializationError;

    KAOS_LOGI(__FUNCTION__, "Run setup");

    err = run_setup(registry);
    if (err) {
        destroy_registry(registry);
        return KaosModuleInitializationError;
    }

    KAOS_LOGI(__FUNCTION__, "Done");

    return KaosSuccess;
}


kaos_error_t execute_function(module_registry_t *registry, char *function_name, uint32_t *function_args, uint32_t arg_n, uint32_t op_stack_sz) {
    module_inst_t module_inst = get_container_inst(registry);
    wasm_function_inst_t func = wasm_runtime_lookup_function(module_inst, (char *) function_name);

    if (!func) {
        KAOS_LOGE(MGR_TAG, "Failed to lookup function %s", function_name);
        return KaosModuleExecutionError;
    }

    exec_env_t exec_env;
    if (
        !(exec_env = wasm_runtime_create_exec_env(module_inst, op_stack_sz))) {
        KAOS_LOGE(MGR_TAG, "Failed to create exec_env");
        return KaosModuleExecutionError;
    }

    set_exec_env(registry, exec_env);
    set_status(registry, RUNNING);

    if (!wasm_runtime_call_wasm(exec_env, func, arg_n, function_args)) {
        KAOS_LOGE(MGR_TAG, "Container %s function call error: %s\n", registry->module_name, wasm_runtime_get_exception(module_inst));
        set_status(registry, ERROR);
        destroy_exec_env(exec_env);

        return KaosModuleExecutionError;
    }
    set_status(registry, SUSPENDED);
    destroy_exec_env(exec_env);
    set_exec_env(registry, exec_env);

    return KaosSuccess;
}


void soft_shutdown() {
    destroy_module_registries();
    destroy_runtime();
}

unsigned char __aligned(4) *copy_buffer(unsigned char *module_buffer, size_t buffer_sz) {
    unsigned char __aligned(4) *new_buffer = calloc(1, buffer_sz);
    memcpy(new_buffer, module_buffer, buffer_sz);

    return new_buffer;
}


void print_interface(interface_config_t interface) {
    KAOS_LOGI(__FUNCTION__, "Identities: %"PRIu16"", interface.n_identities);
    if (interface.n_identities != (int16_t) -1) {
        for (int i = 0; i < interface.n_identities; i++) {
            KAOS_LOGI(__FUNCTION__, "    service: %"PRIu8", address: %"PRIu32", type: %s\n", (interface.identities + i)->service_id, (interface.identities + i)->address, (interface.identities + i)->type);
        }
    }

    if (interface.n_inputs != (int16_t) -1) {
        KAOS_LOGI(__FUNCTION__, "Inputs: %"PRIu16"", interface.n_inputs);
        for (int i = 0; i < interface.n_inputs; i++) {
            KAOS_LOGI(__FUNCTION__, "    service: %"PRIu8", address: %"PRIu32", type: %s\n", (interface.inputs + i)->service_id, (interface.inputs + i)->address, (interface.inputs + i)->type);
        }
    }

    if (interface.n_outputs != (int16_t) -1) {
        KAOS_LOGI(__FUNCTION__, "Outputs: %"PRIu16"", interface.n_outputs);
        for (int i = 0; i < interface.n_outputs; i++) {
            KAOS_LOGI(__FUNCTION__, "    service: %"PRIu8", address: %"PRIu32", type: %s\n", (interface.outputs + i)->service_id, (interface.outputs + i)->address, (interface.outputs + i)->type);
        }
    }

    if (interface.n_resources != (int16_t) -1) {
        KAOS_LOGI(__FUNCTION__, "Resources: %"PRIu16"", interface.n_resources);
        for (int i = 0; i < interface.n_resources; i++) {
            KAOS_LOGI(__FUNCTION__, "    symbol: %s\n", (interface.resources + i)->symbol);
        }
    }
}


void print_config(module_config_t config) {
    KAOS_LOGI(__FUNCTION__, "Source buffer size: %"PRIu32"", config.source_buffer_size);
    KAOS_LOGI(__FUNCTION__, "Source buffer: %p", config.source_buffer);
    KAOS_LOGI(__FUNCTION__, "Heap size: %"PRIu32"", config.heap_size);
    KAOS_LOGI(__FUNCTION__, "Stack size: %"PRIu32"", config.stack_size);
}

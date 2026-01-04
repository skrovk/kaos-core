#ifndef CONTAINER_MGR_H
#define CONTAINER_MGR_H

#include <inttypes.h>
#include <pthread.h>

#include "esp_timer.h"

#include "wamr_wrap.h"
#include "unreliable_message_queue.h"
#include "kaos_signals.h"

#define OK 0
// TODO: Add to config when changing to array implementation
#define KAOS_MAX_MODULE_N CONFIG_KAOS_MAX_MODULE_N

#define KAOS_MODULE_NAME_MAX_SZ       CONFIG_KAOS_MODULE_NAME_MAX_SZ
#define KAOS_MODULE_TIMERS_N          CONFIG_KAOS_MODULE_TIMERS_N

// TODO: Add to config
#define SETUP_FUNC_NAME "setup"
#define SETUP_FUNC_SIG "(i,i,i,i,i,i)"
#define SETUP_FUNC_ARG_N 6


// Address of module reference
// typedef uint32_t module_handle_t;
// TODO: are native symbols part of whole runtime??
typedef struct module_config_t {
    uint8_t *source_buffer;
    uint32_t source_buffer_size;
    uint32_t stack_size;
    uint32_t heap_size;
    wamr_init_args_t *init_args;
} module_config_t;


// TODO: Refactor
typedef struct channel_t {
    service_id_t service_id;
    address_t address;
    char type[TYPE_SZ];
    queue_t queue;
} channel_t;


typedef struct container_channel_t {
    service_id_t service_id;
    address_t address;
    char type[TYPE_SZ];
} container_channel_t;

// refer to this struct when getting signal queue
// get ref to this for other modules too

typedef struct interface_config_t {
    int16_t n_identities;
    channel_t *identities;
    int16_t n_inputs;
    channel_t *inputs;
    int16_t n_outputs;
    channel_t *outputs;
    int16_t n_resources;
    export_symbol_t *resources;
} interface_config_t;

struct signal_queue_t;
struct queue_registry_t;

typedef enum status_t {
    EMPTY,
    SUSPENDED,
    RUNNING,
    ERROR,
} status_t;

typedef struct module_registry_t {
    pthread_mutex_t mutex;
    pthread_mutexattr_t mutex_attr;
    char module_name[KAOS_MODULE_NAME_MAX_SZ + 1];
    module_t module_handle;
    module_inst_t module_inst;
    exec_env_t exec_env;
    module_config_t module_config;
    interface_config_t intfc_config;
    queue_t signal_queue_to_container;
    kaos_timer_t timers[KAOS_MODULE_TIMERS_N];
    status_t status;
    pthread_t thread_id;
} module_registry_t;


typedef struct module_identity_t {
    service_id_t service_id;
    address_t address;
} identity_t;

void init_registry_pool(void);
module_registry_t *get_registry_pool(void);
module_registry_t *get_registry_by_name(char *module_name);
module_registry_t *get_registry_by_handle(module_t module_handle);
module_registry_t *get_registry_by_inst(module_inst_t module_inst);
module_registry_t *get_registry_by_identity(service_id_t service_id, address_t address);
module_registry_t *get_registry_by_sig_handle(queue_t handle);

int assign_timer_id(module_registry_t *registry);
kaos_error_t release_timer_id(module_registry_t *registry, int timer_id);

kaos_timer_t get_timer(module_registry_t *registry, int timer_id);
void set_timer(module_registry_t *registry, kaos_timer_t handle, int timer_id);
kaos_error_t destroy_channel(channel_t channel);
void print_channel(channel_t channel);
channel_t *fetch_channel(channel_t *channels, int16_t n_channels, service_id_t service_id, address_t address, char *type);
char *get_module_name(module_registry_t *registry);
void set_signal_queue(module_registry_t *registry, queue_t to_container);
queue_t get_signal_queue_to_container(module_registry_t *registry);
void set_status(module_registry_t *registry, status_t status);
status_t get_status(module_registry_t *registry);
module_inst_t get_container_inst(module_registry_t *registry);
kaos_error_t set_input_queue(module_registry_t *registry, service_id_t service_id, address_t address, char *type, queue_t queue);
kaos_error_t set_output_queue(module_registry_t *registry, service_id_t service_id, address_t address, char *type, queue_t queue);
queue_t get_input_queue(module_registry_t *registry, service_id_t service_id, address_t address, char *type);
queue_t get_output_queue(module_registry_t *registry, service_id_t service_id, address_t address, char *type);
module_config_t get_module_config(module_registry_t *registry);
interface_config_t get_interface_config(module_registry_t *registry);
void set_thread_id(module_registry_t *registry, pthread_t thread_id);
pthread_t get_thread_id(module_registry_t *registry);

kaos_error_t init_runtime();
kaos_error_t init_module(module_registry_t **registry_dest, char *module_name, module_config_t *config, interface_config_t *interface);
kaos_error_t reinit_module(module_registry_t *registry, char *module_name, module_config_t *config, interface_config_t *interface);
kaos_error_t execute_function(module_registry_t *registry, char *function_name, uint32_t *function_args, uint32_t arg_n, uint32_t op_stack_sz);
void destroy_module(module_registry_t *registry);
kaos_error_t suspend_module(module_registry_t *registry);
void suspend_module_cleanup(module_registry_t *registry);
void soft_shutdown();


unsigned char __aligned(4) *copy_buffer(unsigned char *module_buffer, size_t buffer_sz);
void print_config(module_config_t config);
void print_interface(interface_config_t interface);

#endif

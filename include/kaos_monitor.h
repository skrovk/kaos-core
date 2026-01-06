#ifndef KAOS_MONITOR_H
#define KAOS_MONITOR_H

#include <inttypes.h>
#include <pthread.h>

#include "port.h"
#include "container_mgr.h"
#include "kaos_types_shared.h"
#include "kaos_signals.h"
#include "wamr_wrap.h"
#include "container_mgr.h"


#define N_MOVING_AVG_SAMPLES (1 << 3)

typedef enum BeaconOp {
    LoadNewModule = 1,
    ReloadModule = 2,
    SuspendModule = 3,
    DestroyModule = 4,
    TestModule = 5,
} BeaconOp;


typedef enum BeaconReport {
    OperationSuccessResponse = 1,
    OperationFailureResponse = 2,
    ModuleInfo = 3,
    PlatformInfo = 4,
    ModuleFailure = 5,
    PlatformFailure = 6,
} BeaconReport;

// TODO: Organise by priority
typedef enum event_type_t {             // | event payload        | event payload size 
    EVENT_SIGNAL_TIMER_EXPIRED,

    EVENT_MSG_RECEIVED,
    EVENT_PARSING_DONE,
    EVENT_SIGNAL_RECEIVED,
    EVENT_ISR,                          // | module_registry_t *
    
    EVENT_OPERATION_STARTED,
    EVENT_OPERATION_DONE,               // | op_id_t              | sizeof(op_id_t)
    EVENT_OPERATION_ERROR,              // | op_id_t              | sizeof(op_id_t)
    EVENT_OPERATION_TIMER_EXPIRED,      // | kaos_timer_handle_t * | sizeof(kaos_timer_handle_t *)
    
    EVENT_CONTAINER_LIVE,
    EVENT_CONTAINER_EXEC_TERMINATED,    // | module_registry_t *  | sizeof(module_registry_t *)
    EVENT_CONTAINER_TRAP_SET,
    EVENT_CONTAINER_SUSPENDED,          // | module_registry_t *  | sizeof(module_registry_t *)
    EVENT_CONTAINER_DESTROYED,          // | module_registry_t *  | sizeof(module_registry_t *)
    EVENT_CONTAINER_SIGNAL,
    EVENT_CONTAINER_SIGNAL_CANCELLED,

    EVENT_N
} event_type_t;

typedef enum {
    STATE_OPERATION_INITIALISED = -1,

    STATE_MESSAGE_RECEIVED,
    STATE_MESSAGE_PARSE,
    STATE_ENTER_OPERATION,
    STATE_OPERATION_DONE,
    STATE_OPERATION_ERROR,

    STATE_CHILD_DONE,
    STATE_CHILD_ERROR,

    STATE_MESSAGE_RESPONSE_READY,
    STATE_AWAIT_CHILD,
    STATE_AWAIT_SUSPEND,
    STATE_AWAIT_LOAD,
    // STATE_DONE,
    // STATE_FAILED,
    STATE_CONTAINER_LOAD,
    STATE_CONTAINER_RELOAD,
    // Suspend states
    STATE_SUSPEND,
    STATE_SUSPEND_AWAIT_ACTION,
    STATE_SUSPEND_FORCE,
    STATE_SUSPEND_AWAIT_TRAP,
    STATE_SUSPEND_CLEANUP,
    STATE_DESTROY,
    STATE_DESTROY_ACTION,
    // Load states
    STATE_LOAD_CONTAINER,

} operation_state_t;


typedef struct {
    event_type_t event;
    void *buffer;
    uint32_t buffer_sz;
    int64_t timestamp;
} event_msg_t;

typedef int8_t op_id_t;

typedef struct command_t {
    uint8_t *buf;
    uint32_t sz;
    BeaconOp op;
} command_t;

typedef struct msg_data_t {
    uint8_t *buffer;
    uint32_t buffer_sz;
} msg_data_t;

typedef struct exec_args_t {
    module_config_t config;
    interface_config_t interface;
    char *name;
    module_registry_t *registry;
} exec_args_t;

typedef int32_t isr_id_t ;

typedef struct isr_data_t {
    module_registry_t *registry;
    isr_id_t id;
} isr_data_t;

typedef struct health_monitor_t {
    uint32_t prng_state;
    uint32_t event_sampling_rate;
    struct event_q_latency {
        int64_t mean_vals[N_MOVING_AVG_SAMPLES];
        int64_t avg;
        int next;
        bool cold: 1;
    } event_q_latency;
    int8_t max_queued_events;
    int32_t n_events_dropped;
} health_monitor_t;

#if CONFIG_KAOS_MEASUREMENT_EXEC_LATENCY
typedef struct latency_measurements_t {

} latency_measurements_t;

#endif

#if CONFIG_KAOS_MEASUREMENT_OPERATION

typedef struct op_measurements_t {
    int64_t load_mean_completion;
    int64_t suspend_mean_completion;
    int64_t reload_mean_completion;
    int64_t destroy_mean_completion;
} op_measurements_t;

#endif 


extern export_symbol_t kaos_native_symbols[];

kaos_error_t kaos_run(void);
kaos_error_t submit_event(event_type_t event, void *buffer, uint32_t buffer_sz);
kaos_error_t submit_event_from_ISR(event_type_t event, void *buffer, uint32_t buffer_sz);

#endif
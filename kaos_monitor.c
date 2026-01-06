#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_timer.h"
#include "wasm_export.h"

#include "cbor_parser.h"
#include "container_mgr.h"
#include "kaos_errors.h"
#include "kaos_signals.h"
#include "kaos_monitor.h"
#include "network.h"
#include "led.h"
#include "kaos_button.h"
#include "unreliable_channel.h"

#include "port_logging.h"

#define KAOS_SUSPEND_RETRIES     CONFIG_KAOS_SUSPEND_RETRIES
#define KAOS_SUSPEND_TIMEOUT     CONFIG_KAOS_SUSPEND_TIMEOUT
#define KAOS_OPERATION_TIMEOUT   30000000
#define HANDLE_N 5;

#define KAOS_STATIC_IP_A  CONFIG_KAOS_STATIC_IP_A
#define KAOS_STATIC_IP_B  CONFIG_KAOS_STATIC_IP_B

extern unsigned char __aligned(4) test_wasm[];
volatile TaskStatus_t *pxTaskStatusArray[2];
volatile UBaseType_t uxArraySize[2];

// void get_runtime_stats(int ind, int print_flag) {
//     // int heap_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
//     // int heap_total_perc = heap_total;
//     // int heap_sz = heap_caps_get_free_size(MALLOC_CAP_8BIT);
//     // KAOS_LOGE("MEASUREMENT", "Used heap (B, %%): %d, %f",  heap_total - heap_sz, ((double) heap_total - (double) heap_sz) /(double) heap_total_perc);
//     // KAOS_LOGE("MEASUREMENT", "%d: Used heap, total heap (B, B): %d, %d", milestone, heap_total - heap_sz, heap_total);

//     // TaskStatus_t *pxTaskStatusArray;
//     volatile UBaseType_t uxArraySize, x;
//     unsigned long ulTotalRunTime, ulStatsAsPercentage, idle_task_total_runtime = 0;

//     /* Make sure the write buffer does not contain a string. */
//     uxArraySize = uxTaskGetNumberOfTasks();
//     // KAOS_LOGE("MEASUREMENT", "%d: Tasks running: %d", milestone, uxArraySize);
//     pxTaskStatusArray[ind] = pvPortMalloc( uxArraySize * sizeof( TaskStatus_t ) );

//     if( pxTaskStatusArray[ind] == NULL ) {
//         return;
//     }
//     // uxArraySize = uxTaskGetSystemState( pxTaskStatusArray, uxArraySize, &ulTotalRunTime );

//     /* For percentage calculations. */
//     ulTotalRunTime /= 100UL;
//     /* Avoid divide by zero errors. */
//     if( ulTotalRunTime == 0 ) {
//         return;
//     }

//     for ( x = 0; x < uxArraySize; x++ ) {

//     /* What percentage of the total run time has the task used?
//         This will always be rounded down to the nearest integer.
//         ulTotalRunTimeDiv100 has already been divided by 100. */
//         ulStatsAsPercentage = pxTaskStatusArray[ind][ x ].ulRunTimeCounter / ulTotalRunTime;
//         // KAOS_LOGE(__FUNCTION__, "Task %s:  %lu %%", pxTaskStatusArray[ind][ x ].pcTaskName, ulStatsAsPercentage);

//         if ((!strcmp(pxTaskStatusArray[ind][ x ].pcTaskName, "IDLE0")) || (!strcmp(pxTaskStatusArray[ind][ x ].pcTaskName, "IDLE1") != 0)) {
//             // KAOS_LOGE("MEASUREMENT", "Task runtime: %lu, %lu", pxTaskStatusArray[x].ulRunTimeCounter, ulTotalRunTime);
//             idle_task_total_runtime += pxTaskStatusArray[ind][ x ].ulRunTimeCounter;
//         }


//     }


//         // KAOS_LOGE("MEASUREMENT", "%d: Idle task runtime ([u], %%): %lu, %f", milestone, idle_task_total_runtime, (double) idle_task_total_runtime / ((double) 2 * (double) ulTotalRunTime));
        
//         /* The array is no longer needed, free the memory it consumes. */

//         vPortFree( pxTaskStatusArray[ind] );

//     }


void get_runtime_states(int ind) {
    // TaskStatus_t *pxTaskStatusArray;
    volatile UBaseType_t uxArraySize, x;
    unsigned long ulTotalRunTime, ulStatsAsPercentage;

    // *pcWriteBuffer = 0x00;
    uxArraySize = uxTaskGetNumberOfTasks();
    pxTaskStatusArray[ind] = pvPortMalloc( uxArraySize * sizeof( TaskStatus_t ) );

    if  (pxTaskStatusArray[ind] != NULL) {
        /* Generate raw status information about each task. */
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray[ind], uxArraySize, &ulTotalRunTime);
        /* For percentage calculations. */
        ulTotalRunTime /= 100UL;
        /* Avoid divide by zero errors. */
        if (ulTotalRunTime > 0)

    
        for( x = 0; x < uxArraySize; x++ ) {

            /* What percentage of the total run time has the task used?

            This will always be rounded down to the nearest integer.

            ulTotalRunTimeDiv100 has already been divided by 100. */

            ulStatsAsPercentage = pxTaskStatusArray[ind][ x ].ulRunTimeCounter / ulTotalRunTime;

            // if( ulStatsAsPercentage > 0UL ) {
            // sprintf( pcWriteBuffer, "%stt%lutt%lu%%rn",

            //                     pxTaskStatusArray[ind][ x ].pcTaskName,

            //                     pxTaskStatusArray[ind][ x ].ulRunTimeCounter,

            //                     ulStatsAsPercentage );

            // }

            // else

            // {

            // /* If the percentage is zero here then the task has

            //     consumed less than 1% of the total run time. */

            // sprintf( pcWriteBuffer, "%stt%lutt<1%%rn",

            //                     pxTaskStatusArray[ind][ x ].pcTaskName,

            //                     pxTaskStatusArray[ind][ x ].ulRunTimeCounter );

            // }


            // pcWriteBuffer += strlen( ( char * ) pcWriteBuffer );

        }

        


        /* The array is no longer needed, free the memory it consumes. */

        vPortFree( pxTaskStatusArray[ind] );

    }
    }

void print_runtime_stats(void) {
    for (int i = 0; i < 2; i++) {
        KAOS_LOGE("MEASUREMENT", "%d: Tasks running at %d: %d", i, uxArraySize);
        for (int j = 0; j < uxArraySize[i]; j++) {
            KAOS_LOGI(__FUNCTION__, "Task %s:  %lu %%", pxTaskStatusArray[i][ j ].pcTaskName, pxTaskStatusArray[i][ j ].ulRunTimeCounter);
        }
    }
}

// Monitor containers running
// Receive and execute commands from controller
//  Parse commands
//  Load up containers
//  Update containers
//
// Test if all symbols can be linked initiallly?
export_symbol_t kaos_native_symbols[] = {
    {
        "get",
        get,
        "(ii$)i",
        NULL
    },
    {
        "put",
        put,
        "(iii$*~)i",
        NULL
    },
    {
        "delay",
        delay,
        "(i)",
        NULL
    },
    {
        "rand",
        rand,
        "()i",
        NULL
    },
    {
        "sig_from_kaos",
        sig_from_kaos,
        "(i)I",
        NULL
    },
    {
        "sig_to_kaos",
        sig_to_kaos,
        "(iiI)i",
        NULL
    },
    {
        "claim_red",
        claim_red,
        "()i",
        NULL
    },
    {
        "claim_blue",
        claim_blue,
        "()i",
        NULL
    },
    {
        "claim_green",
        claim_green,
        "()i",
        NULL
    },
    {
        "surrender_red",
        surrender_red,
        "()i",
        NULL
    },
    {
        "surrender_blue",
        surrender_blue,
        "()i",
        NULL
    },
    {
        "surrender_green",
        surrender_green,
        "()i",
        NULL
    },
    {
        "register_button",
        register_button,
        "(i)i",
        NULL
    }
    // {
    //     "get_runtime_stats",
    //     get_runtime_stats,
    //     "(i)",
    //     NULL
    // }
};

static health_monitor_t monitor = {
    .prng_state = 0xF0CACC1A,
    .event_sampling_rate = 16,
    .max_queued_events = 0,
    .n_events_dropped = 0,
    .event_q_latency = {
        .mean_vals = {0},
        .avg = 0,
        .next = 0,
        .cold = 1,
    },
};

static queue_t *main_event_queue;

// Initialize queues
// TODO: Msg size limits
static kaos_error_t init_monitor(uint32_t max_buff_sz) {
    main_event_queue = create_queue(10, sizeof(event_msg_t));
    if (!main_event_queue) return KaosQueueNotEstablished;

    return 0;
}


static kaos_error_t unpack_command(uint8_t *buffer, uint32_t buffer_sz, command_t *msg) {
    memset(msg, 0, sizeof(command_t));
    
    if (buffer_sz < (2 *sizeof(uint32_t))) {
        KAOS_LOGW(__FUNCTION__, "Invalid message: %"PRIu32" too short", buffer_sz);
        return KaosInvalidCommandError;
    }
    memcpy(&(msg->op), buffer, sizeof(uint32_t));
    memcpy(&(msg->sz), buffer + sizeof(uint32_t), sizeof(uint32_t));
    // KAOS_LOGI(__FUNCTION__, "Msg data, Operation %"PRIu32", size %"PRIu32"", msg->op, msg->sz);

    if (!msg->sz) {
        msg->buf = NULL;  // Ensure buf is NULL when sz is 0
        return KaosSuccess;
    }

    // ESP_LOG_BUFFER_HEX(__FUNCTION__, buffer, msg->sz);
    msg->buf = (uint8_t *) calloc(1, msg->sz);
    if (!msg->buf) {
        KAOS_LOGW(__FUNCTION__, "Allocation failed, %"PRIu32", %"PRIu32"", msg->op, msg->sz);
        return KaosContainerMemoryAllocationError;
    }

    memcpy(msg->buf, buffer + 2*sizeof(uint32_t), msg->sz);
    return KaosSuccess;
}

static uint32_t xorshift32(uint32_t state) {
    uint32_t x = state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;

	return state = x;
}

// TODO: Change buffer to void *
kaos_error_t submit_event(event_type_t event, void *buffer, uint32_t buffer_sz) {
    KAOS_LOGI(__FUNCTION__, "Submitting event %d", event);
    event_msg_t event_msg = {
        .event = event,
        .buffer = buffer,
        .buffer_sz = buffer_sz
    };

    uint32_t sample = xorshift32(monitor.prng_state);
    uint32_t mask = (1u << monitor.event_sampling_rate) - 1u;

    if (sample & mask) {
        event_msg.timestamp = get_time_us();
    }
    
    if (!send_to_queue(main_event_queue, &event_msg, 0)) {
        KAOS_LOGE(__FUNCTION__, "Main event queue is full");
        // TODO: increment n_main_events_dropped
        monitor.n_events_dropped++;
        return KaosQueueFullError;
    }

    return KaosSuccess;
}


// TODO: Store event pointers instead of copy? Make sure costs is kept down
kaos_error_t submit_event_from_ISR(event_type_t event, void *buffer, uint32_t buffer_sz) {
    event_msg_t event_msg = {
        .event = event,
        .buffer = buffer,
        .buffer_sz = buffer_sz
    };

    send_to_queue_from_ISR(main_event_queue, &event_msg);

    return KaosSuccess;
}


static void free_exec_args(exec_args_t *exec_args) {
    if (!exec_args) return;
    if (exec_args->config.source_buffer) free(exec_args->config.source_buffer);
    if (exec_args->interface.identities) free(exec_args->interface.identities);
    if (exec_args->interface.inputs) free(exec_args->interface.inputs);
    if (exec_args->interface.outputs) free(exec_args->interface.outputs);
    if (exec_args->interface.resources) free(exec_args->interface.resources);
    free(exec_args);
}


static void *startup_container(void *arg) {
    wasm_runtime_init_thread_env();
    exec_args_t *exec_args = (exec_args_t *) arg;

    // module_registry_t *registry = NULL;
    kaos_error_t err;

    if (!exec_args->registry) {
        // init_runtime();

        /* Verify heap integrity before handing received buffer to monitor */
        if (!heap_caps_check_integrity_all(true)) {
            KAOS_LOGE(__FUNCTION__, "Heap corruption detected after network receive");
            return NULL;
        }

        err = init_module(&exec_args->registry, exec_args->name, &exec_args->config, &exec_args->interface);
    } else {
        err = reinit_module(exec_args->registry, exec_args->name, &exec_args->config, &exec_args->interface);
    }

    

    KAOS_LOGI(__FUNCTION__, "Load registry %p", exec_args->registry);
    // Measurement 2. Upon task initialization
    // get_runtime_stats(NULL, 2);

    if (err) {
        KAOS_LOGE(__FUNCTION__, "%s Initialization error %d", exec_args->name, err);
        submit_event(EVENT_CONTAINER_EXEC_TERMINATED, NULL, 0);
        return NULL;
    }

    if (!exec_args->registry) {
        KAOS_LOGW(__FUNCTION__, "Module not initialized");
        submit_event(EVENT_CONTAINER_EXEC_TERMINATED, NULL, 0);
        // TODO: free args and resources inside
        return NULL;
    }

    // TODO: Supply return value from main to submitted event  
    module_config_t config = get_module_config(exec_args->registry);
    uint32_t return_arg[2] = {0};
    err = execute_function(exec_args->registry, "main_app", return_arg, 0, config.stack_size);

    KAOS_LOGI(__FUNCTION__, "main_app: returned %d,%d", return_arg[0], return_arg[1]);

    if (err) {
        set_status(exec_args->registry, ERROR);
        KAOS_LOGE(__FUNCTION__, "%s Execute error %d", exec_args->name, err);
        submit_event(EVENT_CONTAINER_EXEC_TERMINATED, NULL, 0);
        return NULL;
    } else {
        set_status(exec_args->registry, SUSPENDED);
        submit_event(EVENT_CONTAINER_EXEC_TERMINATED, NULL, 0);
    }

    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(__FUNCTION__, "Heap integrity check failed before wasm_runtime_load");
        return NULL;
    }

    // TODO: !get function return value
    wasm_runtime_destroy_thread_env();
    // TODO: Check for running suspend timer and cancel it?
    // set_status(exec_args->registry, SUSPENDED);

    // TODO: include function return value
    // char *event_buffer = calloc(1, sizeof(module_registry_t *));
    // memcpy(event_buffer, &registry, sizeof(module_registry_t *));
    // TODO: default behaviour when container function is over, destroy registry?

    return NULL;
}


static kaos_error_t exec_container(exec_args_t *exec_args) {
    // TODO: check existing id container
    KAOS_LOGI(__FUNCTION__, "Loading container: %s", exec_args->name);
    exec_args_t *args = calloc(1, sizeof(exec_args_t));
    if (!args) {
        KAOS_LOGE(__FUNCTION__, "Exec args allocation failed");
        return KaosMemoryAllocationError;
    }
    memcpy(args, exec_args, sizeof(exec_args_t));

    pthread_t t_1;
    pthread_attr_t tattr_1;

    pthread_attr_init(&tattr_1);
    pthread_attr_setdetachstate(&tattr_1, PTHREAD_CREATE_JOINABLE);
    // TODO: Set stack size
    pthread_attr_setstacksize(&tattr_1, 6 * 1024);

    int pthread_err;
    pthread_err = pthread_create(&t_1, &tattr_1, startup_container, (void *) args);
    assert(pthread_err == 0);

    return KaosSuccess;
}


static export_symbol_t *match_export(char *symbol_name) {
    size_t n_symbols = sizeof(kaos_native_symbols) / sizeof(kaos_native_symbols[0]);
    for (size_t i = 0; i < n_symbols; i++) {
        if (!strcmp(kaos_native_symbols[i].symbol, symbol_name)) {
            return &kaos_native_symbols[i];
        }
    }

    return NULL;
}

static int match_exports(export_symbol_t *exports, int16_t n_exports) {
    if (n_exports == (int16_t) -1) return KaosSuccess;

    for (int i = 0; i < n_exports; i++) {
        export_symbol_t *matched_export = match_export((char *) (exports + i)->symbol);
        if (!matched_export) {
            KAOS_LOGE(__FUNCTION__, "Module configuration symbol %s does not match any available symbols", (char *) (exports + i)->symbol);
            return KaosModuleConfigError;
        }

        // free((uint8_t *) (exports + i)->symbol);
        memcpy(exports + i, matched_export, sizeof(export_symbol_t));
    }

    return KaosSuccess;
}


static int validate_load(exec_args_t *args) {
    if (!args->interface.n_identities) {
        return KaosModuleConfigError;
    }

    if (!args->config.source_buffer_size) {
        return KaosModuleConfigError;
    }

    return 0;
}

static int validate_reload(exec_args_t *args) {
    if (!args->registry) {
        return KaosRegistryNotFoundError;
    }

    if (!args->config.source_buffer_size) {
        return KaosModuleConfigError;
    }

    return 0;
}


typedef enum operation_slot_state_t {
    SLOT_FREE,
    SLOT_BUSY,
    SLOT_ERROR
} operation_slot_state_t;


typedef struct op_ctx_t {
    operation_slot_state_t op_state;
    kaos_timer_handle_t *timer;
    void (*state_transition_callback) (struct op_ctx_t* op_ctx, event_msg_t* event_msg);
    operation_state_t current_state;
    op_id_t op_id;
    op_id_t await_id;
    void *ctx_data;
} op_ctx_t;


#define OPERATION_N 10
static op_ctx_t operations[OPERATION_N];


// Create operation with initial current state value and callback parameter
// Return operation array index
static int8_t add_operation(op_ctx_t op_ctx_config) {
    for (int i = 0; i < OPERATION_N; i++) {
        if (operations[i].op_state == SLOT_FREE) {
            operations[i] = op_ctx_config;
            operations[i].op_state = SLOT_BUSY;
            operations[i].op_id = i;
            operations[i].timer = NULL;  // Initialize timer pointer to NULL

            KAOS_LOGI(__FUNCTION__, "Add operation %d", i);
            submit_event(EVENT_OPERATION_STARTED, (void *) operations[i].op_id, sizeof(void *));

            return i;
        }
    }
    
    KAOS_LOGE(__FUNCTION__, "Operation limit reached");
    return -1;
}

static void remove_operation(op_id_t operation_index) {
    memset(operations + operation_index, 0, sizeof(op_ctx_t));
    // operations[operation_index].op_state = SLOT_FREE;
}



static void fail_op(op_ctx_t *op_ctx) {
    KAOS_LOGI(__FUNCTION__, "%d", op_ctx->op_id);
    op_ctx->current_state = STATE_OPERATION_DONE;
    void *buffer = (void *) op_ctx->op_id;
    
    if (submit_event(EVENT_OPERATION_ERROR, buffer, sizeof(op_id_t))) {
        KAOS_LOGE("kaos", "Event dropped (silent error) - Event cannot be submitted");
    }

    // free(op_ctx->ctx_data)signal_msg;
}

static void complete_op(op_ctx_t *op_ctx) {
    KAOS_LOGI(__FUNCTION__, "%d", op_ctx->op_id);
    op_ctx->current_state = STATE_OPERATION_DONE;
    void *buffer = (void *) op_ctx->op_id;

    if (submit_event(EVENT_OPERATION_DONE, buffer, sizeof(op_id_t))) {
        KAOS_LOGE("kaos", "Event dropped (silent error) - Event cannot be submitted");
    }

    // free(op_ctx->ctx_data);
}


// Every operation state machine must raise EVENT_OPERATION_DONE upon succesful completion or EVENT_OPERATION_ERROR 

// Operation state machine error handling
// Raise EVENT_OPERATION_ERROR to notify operations which depend on the failed operation and enter error info into context
// Enter error state on operation which failed, with operation context in posession of 
// error context (op id, error int and 100 byte error message)


// To handle error:
// Check if there is an await id
// If yes, check whether it's in error state
// If not in error state, raise STATE_OPERATION_ERROR on that id, if it's not already in error state


// TODO: Make sure all state machines have an error state, which can be checked against - to relay to parent process 
// TODO: Make sure all state machines transition to error state upon error
// TODO: Clean up queue from stale events - check for operation-specific events and discard them when the process slot is deallocated


static void load_state_transition(op_ctx_t* op_ctx, event_msg_t* event_msg) {
    kaos_error_t err;

    switch (event_msg->event) {
    case EVENT_OPERATION_STARTED:

#if KAOS_MEASUREMENT_OPERATION
#endif

        if(op_ctx->current_state == STATE_OPERATION_INITIALISED) {
            op_ctx->current_state = STATE_CONTAINER_LOAD;
            break;
        }
        fail_op(op_ctx);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    case EVENT_CONTAINER_LIVE:
        if (op_ctx->current_state == STATE_CONTAINER_LOAD) {
            exec_args_t *args = (exec_args_t *) op_ctx->ctx_data;
            module_registry_t *target_registry = args->registry;

            if (((module_registry_t *) event_msg->buffer) == target_registry) {
                KAOS_LOGI(__FUNCTION__, "Module %s now live", args->name);
                complete_op(op_ctx);
                free_exec_args((exec_args_t *) op_ctx->ctx_data);
                op_ctx->ctx_data = NULL;
            }
        }
        return;
    case EVENT_CONTAINER_EXEC_TERMINATED:
        if (op_ctx->current_state == STATE_CONTAINER_LOAD) {
            exec_args_t *args = (exec_args_t *) op_ctx->ctx_data;
            module_registry_t *target_registry = args->registry;

            if (((module_registry_t *) event_msg->buffer) == target_registry) {
                KAOS_LOGI(__FUNCTION__, "Module %s execution terminated before live", args->name);
                fail_op(op_ctx);
                destroy_module(args->registry);
                free_exec_args((exec_args_t *)op_ctx->ctx_data);
                op_ctx->ctx_data = NULL;
                return;
            }
        } 
        // Otherwise it should be done, just terminated very quickly
        
    default: 
        return;
    }
    
    switch (op_ctx->current_state) {
    case STATE_OPERATION_DONE:
        // No action
        break;
    case STATE_CONTAINER_LOAD:
        exec_args_t *args = (exec_args_t *) op_ctx->ctx_data;
        KAOS_LOGI(__FUNCTION__, "Container %s load", args->name);
        err = exec_container(args);

        if (err) {
            KAOS_LOGE(__FUNCTION__, "Container execution failed, operation error");
            fail_op(op_ctx);
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            op_ctx->ctx_data = NULL;
            break;
        } 
        break;
    default:
        KAOS_LOGE(__FUNCTION__, "Invalid state %d", op_ctx->current_state);
        fail_op(op_ctx);
        free_exec_args((exec_args_t *)op_ctx->ctx_data);
        op_ctx->ctx_data = NULL;
        break;
    }
}


static void suspend_timer_expired(void *arg) {
    KAOS_LOGE(__FUNCTION__, "Timer expired");
    submit_event(EVENT_OPERATION_TIMER_EXPIRED, arg, sizeof(op_id_t));
}

// TODO: Implement message for complete/fail op to differentiate outcomes
static void suspend_state_transition(op_ctx_t* op_ctx, event_msg_t* event_msg) { 
    kaos_error_t err;   
    switch (event_msg->event) {
    case EVENT_OPERATION_STARTED:
        if(op_ctx->current_state == STATE_OPERATION_INITIALISED) {
            op_ctx->current_state = STATE_SUSPEND;
            break;
        }
        
        fail_op(op_ctx);
        // free_exec_args(op_ctx->ctx_data);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        break;
    case EVENT_OPERATION_TIMER_EXPIRED:
        if (op_ctx->current_state == STATE_SUSPEND_AWAIT_ACTION) {
            op_ctx->current_state = STATE_SUSPEND_FORCE;
            break;
        }
        
        fail_op(op_ctx);
        // free_exec_args(op_ctx->ctx_data);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        break;
    case EVENT_CONTAINER_EXEC_TERMINATED:
        // TODO: add information about voluntary/forced termination to context?
        if (op_ctx->timer) {
            err = stop_timer(op_ctx->timer);
            if (err) {
                KAOS_LOGE(__FUNCTION__, "Timer stop error %d", err);
                fail_op(op_ctx);
                return;
            }
            err = delete_timer(op_ctx->timer);
            if (err) {
                KAOS_LOGE(__FUNCTION__, "Timer delete error %d", err);
                fail_op(op_ctx);
                return;
            }
    }
        
        if (op_ctx->current_state == STATE_SUSPEND_AWAIT_ACTION) {
            op_ctx->current_state = STATE_SUSPEND_CLEANUP;
            KAOS_LOGI(__FUNCTION__, "Container surrendered to termination willingly");
            break;
        }

        if (op_ctx->current_state == STATE_SUSPEND_AWAIT_TRAP) {
            // TODO: Add differen tmessage for different outcome
            op_ctx->current_state = STATE_SUSPEND_CLEANUP;
            KAOS_LOGW(__FUNCTION__, "Container terminated by force");
            break;
        } 

        fail_op(op_ctx);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    default: 
        // KAOS_LOGE(__FUNCTION__, "Invalid event %d", op_ctx->current_state);
        // fail_op(op_ctx);
        // free_exec_args((exec_args_t *)op_ctx->ctx_data);
        return;
    }

    exec_args_t *args;
    module_registry_t *registry;
    switch (op_ctx->current_state) {
    case STATE_SUSPEND:
        args = (exec_args_t *) op_ctx->ctx_data;
        registry = args->registry;
            
        if (get_status(args->registry) != RUNNING) {
            submit_event(EVENT_CONTAINER_EXEC_TERMINATED, NULL, 0);
            op_ctx->current_state = STATE_SUSPEND_AWAIT_ACTION;
            KAOS_LOGI(__FUNCTION__, "Module not running, cannot suspend");
            break;
        }
        // Set timer to termination delay, add event with callback to event queue to execute suspend and to add response about completion to server
        kaos_timer_args_t *oneshot_timer_args = kaos_timer_args_init(&suspend_timer_expired, (void *) op_ctx->op_id, "suspend");
        // TODO: Handle error
        esp_err_t esp_err =  create_timer(&(op_ctx->timer), oneshot_timer_args);
        kaos_timer_args_free(oneshot_timer_args);
        if (esp_err != ESP_OK) {
            KAOS_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
            fail_op(op_ctx);
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            op_ctx->ctx_data = NULL;
            break;
        }

        esp_err = start_timer(op_ctx->timer, KAOS_SUSPEND_TIMEOUT, 0);
        if (esp_err != ESP_OK) {
            KAOS_LOGE(__FUNCTION__, "ESP Timer error %d", esp_err);
            fail_op(op_ctx);
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            op_ctx->ctx_data = NULL;
            break;
        }
        args = (exec_args_t *) op_ctx->ctx_data;
        registry = args->registry;
        if (!registry) {
            fail_op(op_ctx);
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            op_ctx->ctx_data = NULL;
            KAOS_LOGE(__FUNCTION__, "Missing context in" );
            break;
        }

        // TODO: Make sure this works 
        kaos_signal_msg_t sig_msg = (kaos_signal_msg_t) {
            .signal_type = KAOS_SIGNAL_TERMINATE,
            .data = 0,
        };
        sig_to_container(registry, &sig_msg);
        op_ctx->current_state = STATE_SUSPEND_AWAIT_ACTION;
    case STATE_SUSPEND_AWAIT_ACTION:
        break;
    case STATE_SUSPEND_FORCE:
        args = (exec_args_t *) op_ctx->ctx_data;
        registry = args->registry;
        if (!registry) {
            fail_op(op_ctx);
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            op_ctx->ctx_data = NULL;
            KAOS_LOGE(__FUNCTION__, "Missing context in" );
        }
        KAOS_LOGI(__FUNCTION__, "Timer expired, suspending module by force");
        delete_timer(op_ctx->timer);

        err = suspend_module(registry);
        if (err) {
            fail_op(op_ctx);
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            op_ctx->ctx_data = NULL;
            KAOS_LOGE(__FUNCTION__, "Module trap could not be set");
        }

        op_ctx->current_state = STATE_SUSPEND_AWAIT_TRAP;
        break;
        //TODO: Return message with result code
    case STATE_SUSPEND_AWAIT_TRAP:
        break;
    case STATE_SUSPEND_CLEANUP:
        args = (exec_args_t *) op_ctx->ctx_data;
        registry = args->registry;
        if (!registry) {
            fail_op(op_ctx);
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            op_ctx->ctx_data = NULL;
            KAOS_LOGE(__FUNCTION__, "Missing context in %s");
        }

        suspend_module_cleanup(registry);
        free_exec_args((exec_args_t *)op_ctx->ctx_data);
        op_ctx->ctx_data = NULL;
        complete_op(op_ctx);
        break;
        // TODO: Add differen tmessage for different outcome based on event

    default:
        KAOS_LOGE(__FUNCTION__, "Invalid state %s", op_ctx->current_state);
        fail_op(op_ctx);
        free_exec_args((exec_args_t *)op_ctx->ctx_data);
        op_ctx->ctx_data = NULL;
        break;
    }
}

static void reload_state_transition(op_ctx_t* op_ctx, event_msg_t* event_msg) {
    switch (event_msg->event) {
    case EVENT_OPERATION_STARTED:
        if(op_ctx->current_state == STATE_OPERATION_INITIALISED) {
            op_ctx->current_state = STATE_DESTROY;
            break;
        }
        
        fail_op(op_ctx);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    case EVENT_OPERATION_DONE:   
        op_ctx->await_id = -1;  
        if ((op_ctx->current_state == STATE_AWAIT_SUSPEND)) {
            op_ctx->current_state = STATE_CONTAINER_LOAD;
            break;
        } 

        if ((op_ctx->current_state == STATE_CONTAINER_LOAD)) {
            op_ctx->current_state = STATE_CHILD_DONE;
            break;
        } 
            
        fail_op(op_ctx);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    // Only can catch the event for child operation id?
    case EVENT_OPERATION_ERROR:
        op_ctx->await_id = -1;  
        fail_op(op_ctx);
        if(op_ctx->current_state == STATE_AWAIT_CHILD) { 
            KAOS_LOGE(__FUNCTION__, "Disappointed parent error: Operation failed due to child failure");
            return;
        }

        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
        //  resolve dependent operations -> what if it has failed by now?
        // cancel operation  
    default:
        return;
    }

    op_ctx_t child_ctx;
    switch (op_ctx->current_state) {
        case STATE_CONTAINER_RELOAD:        
            child_ctx = (op_ctx_t) {
                .current_state = STATE_OPERATION_INITIALISED,
                .await_id = -1,
                .state_transition_callback = &suspend_state_transition,
                .timer = NULL,
                .ctx_data = op_ctx->ctx_data
            };

            op_ctx->await_id = add_operation(child_ctx);
            op_ctx->current_state = STATE_AWAIT_SUSPEND;   
            // TODO: set timer for child operation?
            break; 
        case STATE_CONTAINER_LOAD:
            child_ctx = (op_ctx_t) {
                .current_state = STATE_OPERATION_INITIALISED,
                .await_id = -1,
                .state_transition_callback = &load_state_transition,
                .timer = NULL,
                .ctx_data = op_ctx->ctx_data
            };

            op_ctx->await_id = add_operation(child_ctx);
            op_ctx->current_state = STATE_AWAIT_LOAD;   
            // TODO: set timer for child operation?
            break; 
        case STATE_CHILD_DONE:
            complete_op(op_ctx);
            break;
        case STATE_CHILD_ERROR:
            fail_op(op_ctx);
            KAOS_LOGE(__FUNCTION__, "Disappointed parent error: Operation error due to child failure");
            break;
        default:
            KAOS_LOGE(__FUNCTION__, "Invalid state %s", op_ctx->current_state);
            fail_op(op_ctx);
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            break;
    }
}


static void destroy_state_transition(op_ctx_t* op_ctx, event_msg_t* event_msg) {
    // kaos_error_t kaos_error = KaosOperationIncomplete;

    switch (event_msg->event) {
    case EVENT_OPERATION_STARTED:
        if(op_ctx->current_state == STATE_OPERATION_INITIALISED) {
            op_ctx->current_state = STATE_DESTROY;
            break;
        }
        
        fail_op(op_ctx);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    case EVENT_OPERATION_ERROR:
    // Where is error information encoded
        op_ctx->await_id = -1;
        fail_op(op_ctx);
        if(op_ctx->current_state == STATE_AWAIT_CHILD) { 
            KAOS_LOGE(__FUNCTION__, "Disappointed parent error: Operation failed due to child failure");
            return;
        }

        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    case EVENT_OPERATION_DONE:
        op_ctx->await_id = -1;
        if(op_ctx->current_state == STATE_AWAIT_CHILD) { 
            op_ctx->current_state = STATE_DESTROY_ACTION;
            break;
        }

        fail_op(op_ctx);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    default:
        // fail_op(op_ctx);
        // KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    }

    switch (op_ctx->current_state) {
    case STATE_DESTROY:
    // suspend frees exec_args
        exec_args_t *child_ctx_data = calloc(1, sizeof(exec_args_t));
        memcpy(child_ctx_data, op_ctx->ctx_data, sizeof(exec_args_t));
        // launch suspend operation
        op_ctx_t child_ctx = (op_ctx_t) {
            .current_state = STATE_OPERATION_INITIALISED,
            .await_id = -1,
            .state_transition_callback = &suspend_state_transition,
            .timer = NULL,
            .ctx_data = (void *) child_ctx_data
        };
        op_ctx->await_id = add_operation(child_ctx);
        op_ctx->current_state = STATE_AWAIT_CHILD;
        break;
    case STATE_AWAIT_CHILD:
        break;
    case STATE_DESTROY_ACTION:
        exec_args_t *args = (exec_args_t *) op_ctx->ctx_data;
        module_registry_t *registry = args->registry;

        KAOS_LOGI(__FUNCTION__, "Destroying module %s", registry->module_name);
        destroy_module(registry);

        module_registry_t *reg = get_registry_by_name(args->name);  // Should be NULL now
        if (reg) {
            KAOS_LOGE(__FUNCTION__, "Module %s destruction failed", args->name);
            fail_op(op_ctx);
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            break;
        }
        KAOS_LOGI(__FUNCTION__, "Module %s destroyed", args->name);
        complete_op(op_ctx);
        break;
    default:
        KAOS_LOGE(__FUNCTION__, "Invalid state %s", op_ctx->current_state);
        fail_op(op_ctx);
        free_exec_args((exec_args_t *)op_ctx->ctx_data);
        break;
    }
}

static void test_state_transition(op_ctx_t* op_ctx, event_msg_t* event_msg) {
    KAOS_LOGE(__FUNCTION__, "Test operation %d success", op_ctx->op_id);
    complete_op(op_ctx);
}


static kaos_error_t process_command(op_ctx_t* op_ctx, msg_data_t *data) {
    // KAOS_LOGI(__FUNCTION__, "Process message of type %"PRIu32"", msg->op);
    command_t command;
    kaos_error_t err = KaosSuccess;
    command.op = LoadNewModule;
    command.sz = 0;
    command.buf = NULL;
    // kaos_error_t err = unpack_command(data->buffer, data->buffer_sz, &command);
    *op_ctx = (op_ctx_t) {
            .current_state = -1,
            // TODO: set timer here or after parsing
            .state_transition_callback = NULL,
            .await_id = -1,
            .timer = NULL,
            .ctx_data = NULL
        };

    if (err) {
        KAOS_LOGW(__FUNCTION__, "Invalid command");
        return err;
    }

    // TODO: Improve memory management
    exec_args_t *args = calloc(1, sizeof(exec_args_t));
    if (!args) {
        KAOS_LOGE(__FUNCTION__, "Failed to allocate exec_args");
        if (command.buf) free(command.buf);
        return KaosMemoryAllocationError;
    }
    memset(args, 0, sizeof(exec_args_t));
    kaos_error_t result;

    // if ((result = parse_monitor_message(command.buf, command.sz, &(args->config), &(args->interface), &(args->name)))) {
    //     KAOS_LOGW(__FUNCTION__, "Message parsing failed");
    //     if (command.buf) free(command.buf);  // Free the allocated buffer
    //     free_exec_args(args);
    //     return result;
    // }

    /* Verify heap integrity before handing received buffer to monitor */
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(__FUNCTION__, "Heap corruption detected after network receive");
        return KaosMemoryAllocationError;
    }

    char *test_name = "test";
    args->name = (char *) calloc(1, strlen(test_name) + 1);
    if (!args->name) {
        KAOS_LOGE(__FUNCTION__, "Failed to allocate args->name");
        if (command.buf) free(command.buf);
        free_exec_args(args);
        return KaosMemoryAllocationError;
    }
    strncpy(args->name, test_name, strlen(test_name));
    args->name[strlen(test_name)] = '\0';
    KAOS_LOGI(__FUNCTION__, "Module name: %s", args->name);
    // print_interface(args->interface);
    // print_config(args->config);

    // if ((result = match_exports(args->interface.resources, args->interface.n_resources))) {
    //     // TODO: Free resources
    //     if (command.buf) free(command.buf);  // Free the allocated buffer
    //     free_exec_args(args);
    //     return result;
    // }

    if (command.buf) free(command.buf);

    module_registry_t *registry = get_registry_by_name(args->name);

    switch (command.op) {
    case LoadNewModule:
        if (registry) {
            KAOS_LOGW(__FUNCTION__, "Registry conflict with name %s: module already exists", args->name);
            free_exec_args(args);
            return KaosRegistryNameConflictError;
        }

        // if ((result = validate_load(args))) {
        //     KAOS_LOGW(__FUNCTION__, "Load error: %d", result);
        //     free_exec_args(args);
        //     return result;
        // }

        op_ctx->state_transition_callback = &load_state_transition;
        break;
    case ReloadModule:
        args->registry = registry;

        if ((result = validate_reload(args))) {
            KAOS_LOGW(__FUNCTION__, "Reload error: %d", result);
            free_exec_args(args);
            return result;
        }

        if (!registry) {
            KAOS_LOGW(__FUNCTION__, "Registry with name %s not found", args->name);
            free_exec_args(args);

            return KaosRegistryNotFoundError;
        }

        op_ctx->state_transition_callback = &reload_state_transition;
        break;
    case SuspendModule:
        if (!registry) {
            KAOS_LOGW(__FUNCTION__, "Registry with name %s not found", args->name);
            free_exec_args(args);
            return KaosRegistryNotFoundError;
        }
        args->registry = registry;
        op_ctx->state_transition_callback = &suspend_state_transition;
        break;
    case DestroyModule:
        if (!registry) {
            KAOS_LOGW(__FUNCTION__, "Registry with name %s not found", args->name);
            free_exec_args(args);
            return KaosRegistryNotFoundError;
        }

        args->registry = registry;
        op_ctx->state_transition_callback = &destroy_state_transition;
        
        // Measurement 5. After container eliminated
        // KAOS_LOGE_shutting down
        // get_runtime_stats(NULL, 5);
        break;
    case TestModule:
        KAOS_LOGW(__FUNCTION__, "Test module string: %s", args->name);
        op_ctx->state_transition_callback = &test_state_transition;
        break;
    default:
        KAOS_LOGE(__FUNCTION__, "Message invalid");
        free_exec_args(args);
        return KaosInvalidCommandError;
    }

    op_ctx->ctx_data = (void *) args;

    return KaosSuccess;
}

static void op_error(op_ctx_t *op_ctx, char *content) {
    service_msg_t msg = (service_msg_t) {
        .buffer = (uint8_t *) content,
        .buffer_sz = strlen(content) + 1,
    };
    kaos_error_t err = service_put(&msg);
    if (err) {
        KAOS_LOGE(__FUNCTION__, "Operation outcome message not submitted for sending %d", err);
        fail_op(op_ctx);
        return;
    }

    KAOS_LOGI(__FUNCTION__, "Complete operation");
    complete_op(op_ctx);
}

// Create message operation upon receiving message
static void orchestrator_message_state_transition(op_ctx_t* op_ctx, event_msg_t* event_msg) {
    // TODO: Set timer to expire stalling operation
    switch(event_msg->event) {
    case EVENT_OPERATION_STARTED:
        if(op_ctx->current_state == STATE_OPERATION_INITIALISED) {
            op_ctx->current_state = STATE_MESSAGE_PARSE;
            break;
        }
        
        fail_op(op_ctx);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    case EVENT_OPERATION_DONE:    
        // Should only be submitted by the child operation
        if ((op_ctx->current_state == STATE_AWAIT_CHILD)) {
            esp_err_t err = stop_timer(op_ctx->timer);
            if (err) {
                KAOS_LOGE(__FUNCTION__, "Timer stop error %d", err);
                fail_op(op_ctx);
                return;
            } 
            op_ctx->current_state = STATE_CHILD_DONE;
            break;
        } 
            
        fail_op(op_ctx);
        KAOS_LOGE(__FUNCTION__, "Invalid transition with event %d in state %d", event_msg->event, op_ctx->current_state);
        return;
    // Only can catch the event for child operation id?
    case EVENT_OPERATION_ERROR:
    case EVENT_OPERATION_TIMER_EXPIRED:
        op_ctx->current_state = STATE_CHILD_ERROR;
        //  resolve dependent operations -> what if it has failed by now?
        // cancel operation  
        break;
    default:
        return;
    }   

    char *content = NULL;

    switch (op_ctx->current_state) {
    case STATE_MESSAGE_PARSE:        
        op_ctx_t child_op_ctx;
        kaos_error_t err = process_command(&child_op_ctx, (msg_data_t *) op_ctx->ctx_data);
        if (err) {
            KAOS_LOGE(__FUNCTION__, "Invalid command");
            op_error(op_ctx, "Invalid command");
            
            break;
        }

        op_ctx->await_id = add_operation(child_op_ctx);

        if (op_ctx->await_id < 0) {
            op_error(op_ctx, "Child operation could not be instantiated");
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            break;
        }

        // KAOS_LOGE(__FUNCTION__, "Await child %d", op_ctx->await_id);

        op_ctx->current_state = STATE_AWAIT_CHILD;   
        // TODO: set timer for child operation?

        kaos_timer_args_t *oneshot_timer_args = kaos_timer_args_init(&suspend_timer_expired, (void *) op_ctx->op_id, "operation_timeout");
        esp_err_t esp_err =  create_timer(&(op_ctx->timer), oneshot_timer_args);
        kaos_timer_args_free(oneshot_timer_args);
        if (esp_err != ESP_OK) {
            op_error(op_ctx, "Timer error");
            // Child should free exec args
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            free(op_ctx->timer);
            op_ctx->timer = NULL;
            break;
        }

        esp_err = start_timer(op_ctx->timer, KAOS_OPERATION_TIMEOUT, 0);
        if (esp_err != ESP_OK) {
            op_error(op_ctx, "Timer error");
            // Child should free exec args
            free_exec_args((exec_args_t *)op_ctx->ctx_data);
            break;
        }

        break; 
    case STATE_CHILD_DONE:  
        KAOS_LOGI(__FUNCTION__, "Child succeeded");
        content = "Operation carried out successfully";
    case STATE_CHILD_ERROR:
        if (timer_is_active(op_ctx->timer)) {
            esp_err = stop_timer(op_ctx->timer);
            if (esp_err) {
                KAOS_LOGE(__FUNCTION__, "Timer stop error %d", esp_err);
                op_error(op_ctx, "Timer error");
                return;
            } 
        }
        esp_err = delete_timer(op_ctx->timer);
        if (esp_err) {
            KAOS_LOGE(__FUNCTION__, "Timer delete error %d", esp_err);
            op_error(op_ctx, "Timer error");
            return;
        }
        free(op_ctx->timer);
        op_ctx->timer = NULL;

        op_ctx->await_id = -1;
        
        // TODO: Construct message
        if (!content) {
            KAOS_LOGI(__FUNCTION__, "Child failed");
            content = "Operation failed";
        }
        service_msg_t msg = (service_msg_t) {
            .buffer = (uint8_t *) content,
            .buffer_sz = strlen(content) + 1,
        };
        err = service_put(&msg);
        if (err) {
            KAOS_LOGE(__FUNCTION__, "Operation outcome message not submitted for sending %d", err);
            fail_op(op_ctx);
            break;
        }

        KAOS_LOGI(__FUNCTION__, "Complete operation");
        complete_op(op_ctx);
        break;
    default:
        KAOS_LOGE(__FUNCTION__, "Invalid state %d", op_ctx->current_state);
        fail_op(op_ctx);
        break;
    }
}

void handle_signal(kaos_signal_msg_t *signal_msg) {
    if (!is_response(signal_msg->signal_type)) {
        KAOS_LOGW(__FUNCTION__, "Not a response");
        return;
    }

    // TODO: data memory handling
    switch (signal_msg->signal_type ^ 1) {
    case (KAOS_SIGNAL_TERMINATE):
        KAOS_LOGW(__FUNCTION__, "Container acknowledged termination signal %d", signal_msg->signal_id);
        release_signal_id(signal_msg->signal_id);
        break;
    default:
        KAOS_LOGE(__FUNCTION__, "Event response %d not handled");
        break;
    }

    KAOS_LOGE(__FUNCTION__, "Event %d not handled");
}

static void handle_isr_event(isr_data_t *isr_data) {
    kaos_signal_msg_t signal_msg = (kaos_signal_msg_t) {
        .signal_type = KAOS_SIGNAL_ISR,
    };
    memcpy(&(signal_msg.data), &(isr_data->id), sizeof(isr_id_t));
    
    kaos_error_t err =  sig_to_container(isr_data->registry, &signal_msg);
    if (err) {
        KAOS_LOGE(__FUNCTION__, "%d, ISR signal could not be sent to container", err);
    }
}

// TODO: Silent failure when container imports are wrong
// TODO: Validate addresses exchanged between container and kaos
static void monitor_loop(void) {
    for (;;) {
        // TODO: What is the max processing time 
        event_msg_t event_msg;
        
        if (!receive_from_queue(main_event_queue, &event_msg, 10)) {
        // if (!receive_from_queue(main_event_queue, &event_msg, 50000 / portTICK_PERIOD_MS)) {
            // KAOS_LOGE(__FUNCTION__, "nothing");
            continue;
        };
       
        // esp_log_buffer_hex("buffer print", test_wasm, 1618);
        exec_args_t args;
        memset(&args, 0, sizeof(exec_args_t));
        kaos_error_t result;

        if (event_msg.timestamp) {
            int64_t timestamp = get_time_us() - event_msg.timestamp;
            int next =  monitor.event_q_latency.next;

            if (!monitor.event_q_latency.cold) {
                // Should be optimised to shift
                monitor.event_q_latency.avg -= monitor.event_q_latency.mean_vals[next] / (int64_t) N_MOVING_AVG_SAMPLES;
                monitor.event_q_latency.avg += timestamp / (int64_t) N_MOVING_AVG_SAMPLES;
            } else {
                monitor.event_q_latency.avg = (monitor.event_q_latency.avg * (int64_t) next + timestamp) / (int64_t) (next + 1);
            } 

            if (monitor.event_q_latency.cold && (next == (N_MOVING_AVG_SAMPLES - 1))) {
                monitor.event_q_latency.cold = 0;
            }

            monitor.event_q_latency.mean_vals[next] = timestamp;
            monitor.event_q_latency.next = (next++) % N_MOVING_AVG_SAMPLES;
        }
        KAOS_LOGI(__FUNCTION__, "Handle event %d", event_msg.event);

        op_id_t target_id;
        switch (event_msg.event) {
            case EVENT_ISR:
                handle_isr_event((isr_data_t *) event_msg.buffer);
                // TODO: change ownership handling here - how to handle mamory ownerhsip for data passed to callbacks
                event_msg.buffer_sz = 0;
                break;
            case EVENT_SIGNAL_RECEIVED:
                //TODO:  Implement set signal function with timer and callback
                handle_signal((kaos_signal_msg_t *) event_msg.buffer);
                break;
            // Handle operation-specific events
            // Signal reponse timer has expired
            case EVENT_SIGNAL_TIMER_EXPIRED:
            // id is just the index of the operation, you can fetch here
                target_id = (op_id_t) event_msg.buffer;
                event_msg.buffer = NULL;

                // TODO: Error handling for timer expiry, for now just log
                // TODO: Extract timer from context
                delete_timer(operations[target_id].ctx_data); // signal timer here, needs signal id
                KAOS_LOGI(__FUNCTION__, "Signal timer expired");
                break;
            // Operation completion expiry timer has expired
            case EVENT_OPERATION_TIMER_EXPIRED:
                KAOS_LOGI(__FUNCTION__, "Operation timer expired");
            case EVENT_OPERATION_STARTED:
            // TODO: Implement special case for signal and operation timer - container timers are higher importance
                target_id = (op_id_t) event_msg.buffer;
                event_msg.buffer = NULL;
                operations[target_id].state_transition_callback(&(operations[target_id]), &event_msg);
                break;
            // These should be only submitted to the parent operation to process outcome
            case EVENT_OPERATION_DONE:
            case EVENT_OPERATION_ERROR:
                target_id = (op_id_t) event_msg.buffer;
                event_msg.buffer = NULL;
                // TODO: Can operation enter error state with existing operation children?
                if (operations[target_id].await_id < 0) {
                    
                    // Make sure all children are cancelled
                    // If children in error, it will be sorted out
                }

                for (int i = 0; i < OPERATION_N; i++) {
                    if (operations[i].op_state == SLOT_FREE) continue;
                    if (operations[i].op_id == target_id) continue;
                    if (operations[i].await_id == target_id) {
                        operations[i].state_transition_callback(&(operations[i]), &event_msg);
                    }
                }

                KAOS_LOGI(__FUNCTION__, "Remove operation %d", target_id);
                remove_operation(target_id);
                break;
            case EVENT_MSG_RECEIVED:
                msg_data_t *data = calloc(1, sizeof(msg_data_t));
                data->buffer_sz = event_msg.buffer_sz;
                data->buffer = calloc(1, event_msg.buffer_sz);
                memcpy(data->buffer, event_msg.buffer, event_msg.buffer_sz);

                op_ctx_t op_ctx = {
                    .current_state = STATE_OPERATION_INITIALISED,
                    .await_id = -1,
                    .state_transition_callback = &orchestrator_message_state_transition,
                    .timer = NULL,
                    .ctx_data = data
                };

                op_id_t op_id = add_operation(op_ctx);
                if (op_id < 0) {
                    // Free allocated memory on error
                    KAOS_LOGE(__FUNCTION__, "No free operation slots");
                    free(data->buffer);
                    free(data);
                }
                break;
            // Handle operation-agnostic events
            default:
                for (int i = 0; i < OPERATION_N; i++) {
                    if (operations[i].op_state == SLOT_FREE) continue;    
                    operations[i].state_transition_callback(&(operations[i]), &event_msg);
                }
                break;
            // TODO: change implementation to packed array if number of operations is expected to be small

            }
        // TODO: doe this free the wasm buffer memory while hte container is loading? 
            // if (event_msg.buffer_sz) free(event_msg.buffer);
    }
}

kaos_error_t kaos_run(void) {
    // esp_log_level_set("*", ESP_LOG_DEBUG);
// #if KAOS_MEASUREMENTS_ENABLE 
//     esp_log_level_set("*", ESP_LOG_ERROR);
// #endif /* KAOS_MEASUREMENTS_ENABLE */


    // esp_task_wdt_deinit();
    memset(&monitor, 0, sizeof(health_monitor_t));
    memset(operations, 0, sizeof(op_ctx_t) * OPERATION_N);

    monitor.event_q_latency.cold = 1;
    monitor.event_q_latency.next = 0;
    monitor.event_q_latency.avg = 0;

    
    char *ip_addr[2];
    *ip_addr = calloc(1, strlen(KAOS_STATIC_IP_A) + 1);
    *(ip_addr + 1) = calloc(1, strlen(KAOS_STATIC_IP_B) + 1);

    strcpy(*ip_addr, KAOS_STATIC_IP_A);
    strcpy(*(ip_addr + 1), KAOS_STATIC_IP_B);

    // device A
    channel_t channels[] = {
        {
            .service_id = 1,
            .address = 1,
        },
        {
            .service_id = 1,
            .address = 2,
        }
    };

    translation_table_init(channels, (uint8_t **) ip_addr, 2);
    init_registry_pool();
    start_server();

    init_monitor(1024);
    setup_LED();
    init_runtime();

    monitor_loop();


    printf("Exiting...\n");

    return KaosSuccess;
}

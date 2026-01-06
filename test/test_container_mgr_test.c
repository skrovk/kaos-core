

// Provide configuration macros normally supplied by build system
#define CONFIG_KAOS_MAX_MODULE_N 4
#define CONFIG_KAOS_MODULE_NAME_MAX_SZ 31
#define CONFIG_KAOS_MODULE_TIMERS_N 2
#define CONFIG_KAOS_SETUP_STACK_SIZE 256

#include "unity.h"
#include "container_mgr.h"
#include "test_container.h"

#include "esp_heap_caps.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// TODO: Mock pthread?

TEST_CASE("Test module initialisation and destruction", "[container_mgr][exclude]") {
    init_registry_pool();
    uint8_t *src_buffer_copy = calloc(1, sizeof(test_module_buffer));
    memcpy(src_buffer_copy, test_module_buffer, sizeof(test_module_buffer));

    module_config_t config = {
        .source_buffer = src_buffer_copy,
        .source_buffer_size = sizeof(src_buffer_copy),
        .stack_size = 100,
        .heap_size = 100,
        .init_args = NULL
    };
    channel_t identity = { .service_id = 1, .address = 1, .type = "test" };
    interface_config_t iface = {
        .n_identities = 1,
        .identities = &identity,
        .n_inputs = 0,
        .inputs = NULL,
        .n_outputs = 0,
        .outputs = NULL,
        .n_resources = 0,
        .resources = NULL
    };

    module_registry_t *reg = NULL;
    char *name = "test";

    size_t heap_pre_init = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

    kaos_error_t err = init_module(&reg, (uint8_t*) name, &config, &iface);
    TEST_ASSERT_EQUAL(KaosSuccess, err);
    TEST_ASSERT_EQUAL_PTR(reg, get_registry_by_name((uint8_t *) name));
    TEST_ASSERT_EQUAL_PTR(reg, get_registry_by_identity(identity.service_id, identity.address));
    TEST_ASSERT_EQUAL_PTR(NULL, get_registry_by_name((uint8_t *) "wrong_name"));

    destroy_module(reg);

    TEST_ASSERT_EQUAL(heap_pre_init, heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    TEST_ASSERT_EQUAL_PTR(NULL, get_registry_by_name((uint8_t *) name));
    free(src_buffer_copy);
}


TEST_CASE("Initialise registry with name exceeding buffer", "[container_mgr][exclude]") {
    init_registry_pool();

    uint8_t *src_buffer_copy = calloc(1, sizeof(test_module_buffer));
    memcpy(src_buffer_copy, test_module_buffer, sizeof(test_module_buffer));

    module_config_t config = {
        .source_buffer = src_buffer_copy,
        .source_buffer_size = sizeof(src_buffer_copy),
        .stack_size = 100,
        .heap_size = 100,
        .init_args = NULL
    };
    channel_t identity = { .service_id = 1, .address = 1, .type = "test" };
    interface_config_t iface = {
        .n_identities = 1,
        .identities = &identity,
        .n_inputs = 0,
        .inputs = NULL,
        .n_outputs = 0,
        .outputs = NULL,
        .n_resources = 0,
        .resources = NULL
    };

    module_registry_t *reg = NULL;
    char *name_too_long = "this name is way too long";

    size_t heap_pre_init = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

    kaos_error_t err = init_module(&reg, (uint8_t*) name_too_long, &config, &iface);
    TEST_ASSERT_EQUAL(KaosSuccess, err);
    TEST_ASSERT_EQUAL_PTR(NULL, get_registry_by_name((uint8_t*) name_too_long));
    TEST_ASSERT_EQUAL_PTR(reg, get_registry_by_name((uint8_t*) "this name is way to"));

    destroy_module(reg);


    TEST_ASSERT_EQUAL(heap_pre_init, heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    TEST_ASSERT_EQUAL_PTR(NULL, get_registry_by_name((uint8_t*) name_too_long));
    free(src_buffer_copy);
}


// Config copy ----------------------------------------------------------
TEST_CASE("test_copy_buffer", "[container_mgr]") {
    uint8_t buf[4] = {1,2,3,4};
    unsigned char *copy = copy_buffer(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buf, copy, sizeof(buf));
    TEST_ASSERT(copy != buf);
    free(copy);
}

// Channel lookup/set ---------------------------------------------------
TEST_CASE("test_channel_lookup_set", "[container_mgr]") {
    init_registry_pool();

    uint8_t *src_buffer_copy = calloc(1, sizeof(test_module_buffer));
    memcpy(src_buffer_copy, test_module_buffer, sizeof(test_module_buffer));

    module_config_t config = {
        .source_buffer = src_buffer_copy,
        .source_buffer_size = sizeof(src_buffer_copy),
        .stack_size = 100,
        .heap_size = 100,
        .init_args = NULL
    };

    channel_t channels[2];
    channels[0] = (channel_t) { .service_id = 2, .address = 42, .type = "one" };
    channels[1] = (channel_t) { .service_id = 1, .address = 42, .type = "two" };

    channel_t target_success = (channel_t) {
        .service_id = 1,
        .address = 42,
        .type = "two"
    };
    channel_t target_fail = (channel_t) {
        .service_id = 1,
        .address = 42,
        .type = "one"
    };

    channel_t identity = { .service_id = 1, .address = 1, .type = "test" };
    interface_config_t iface = {
        .n_identities = 1,
        .identities = &identity,
        .n_inputs = 2,
        .inputs = channels,
        .n_outputs = 0,
        .outputs = NULL,
        .n_resources = 0,
        .resources = NULL
    };

    module_registry_t *reg = NULL;
    char *name = "test";

    size_t heap_pre_init = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

    kaos_error_t err = init_module(&reg, (uint8_t*) name, &config, &iface);
    TEST_ASSERT_EQUAL(KaosSuccess, err);

    queue_t *q = (queue_t)0x1;
    TEST_ASSERT_EQUAL(KaosSuccess, set_input_queue(reg, target_success.service_id, target_success.address, target_success.type, q));
    TEST_ASSERT_EQUAL_PTR(q, get_input_queue(reg, target_success.service_id, target_success.address, target_success.type));
    TEST_ASSERT_EQUAL_PTR(NULL, get_input_queue(reg, target_fail.service_id, target_fail.address, target_fail.type));
    
    destroy_module(reg);

    TEST_ASSERT_EQUAL(heap_pre_init, heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    TEST_ASSERT_EQUAL_PTR(NULL, get_registry_by_name((uint8_t*) name));
    free(src_buffer_copy);
}


// Null registry and invalid interface ---------------------------------
TEST_CASE("test_null_and_invalid_interface", "[container_mgr]") {
    module_registry_t *reg = NULL;
    module_config_t config = {0};
    interface_config_t iface = { .n_identities = 0, .identities = NULL,
                                 .n_inputs = 0, .inputs = NULL,
                                 .n_outputs = 0, .outputs = NULL,
                                 .n_resources = 0, .resources = NULL };


    TEST_ASSERT_EQUAL(KaosModuleConfigError, init_module(&reg, (uint8_t*) "bad", &config, &iface));
    TEST_ASSERT_EQUAL_PTR(NULL, get_registry_by_name((uint8_t*) "bad"));
}

// // Buffer re-allocation -------------------------------------------------
TEST_CASE("test_realloc_module_buffer", "[container_mgr]") {
    uint8_t *src_buffer_copy = calloc(1, sizeof(test_module_buffer));
    memcpy(src_buffer_copy, test_module_buffer, sizeof(test_module_buffer));

    module_config_t config = {
        .source_buffer = src_buffer_copy,
        .source_buffer_size = sizeof(src_buffer_copy),
        .stack_size = 100,
        .heap_size = 100,
        .init_args = NULL
    };

    module_config_t new_config = {
        .source_buffer = src_buffer_copy,
        .source_buffer_size = sizeof(src_buffer_copy),
        .stack_size = 500,
        .heap_size = 500,
        .init_args = NULL
    };

    channel_t channels[2];
    channels[0] = (channel_t) { .service_id = 2, .address = 42, .type = "one" };
    channels[1] = (channel_t) { .service_id = 1, .address = 42, .type = "two" };

    channel_t new_channels[3];
    new_channels[0] = (channel_t) { .service_id = 2, .address = 42, .type = "two" };
    new_channels[1] = (channel_t) { .service_id = 1, .address = 42, .type = "one" };
    new_channels[2] = (channel_t) { .service_id = 1, .address = 43, .type = "one" };

    channel_t target_success = (channel_t) {
        .service_id = 1,
        .address = 43,
        .type = "one"
    };
    channel_t target_fail = (channel_t) {
        .service_id = 1,
        .address = 42,
        .type = "two"
    };

    channel_t identity = { .service_id = 1, .address = 1, .type = "test" };
    channel_t new_identity = { .service_id = 2, .address = 3, .type = "test" };
    
    interface_config_t iface = {
        .n_identities = 1,
        .identities = &identity,
        .n_inputs = 2,
        .inputs = channels,
        .n_outputs = 0,
        .outputs = NULL,
        .n_resources = 0,
        .resources = NULL
    };

    interface_config_t new_iface = {
        .n_identities = 1,
        .identities = &new_identity,
        .n_inputs = 0,
        .inputs = NULL,
        .n_outputs = 3,
        .outputs = new_channels,
        .n_resources = 0,
        .resources = NULL
    };

    module_registry_t *reg = NULL;
    char *name = "test";

    size_t heap_pre_init = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

    kaos_error_t err = init_module(&reg, (uint8_t*) name, &config, &iface);
    TEST_ASSERT_EQUAL(KaosSuccess, err);

    TEST_ASSERT_EQUAL(KaosSuccess, reinit_module(reg, (uint8_t*) name, &new_config, &new_iface));
    module_config_t cmp_config = get_module_config(reg);

    TEST_ASSERT(new_config.heap_size == cmp_config.heap_size);
    TEST_ASSERT(new_config.stack_size == cmp_config.stack_size);
    // TEST_ASSERT(memcmp(new_cfg.source_buffer, src2, sizeof(src2)) == 0);
    // TEST_ASSERT(new_cfg.source_buffer != old_cfg.source_buffer);
    queue_t *q = (queue_t)0x1;
    TEST_ASSERT_EQUAL(KaosChannelNotFoundError, set_input_queue(reg, target_success.service_id, target_success.address, target_success.type, q));
    TEST_ASSERT_EQUAL_PTR(NULL, get_input_queue(reg, target_success.service_id, target_success.address, target_success.type));

    TEST_ASSERT_EQUAL(KaosSuccess, set_output_queue(reg, target_success.service_id, target_success.address, target_success.type, q));
    TEST_ASSERT_EQUAL_PTR(q, get_output_queue(reg, target_success.service_id, target_success.address, target_success.type));
    TEST_ASSERT_EQUAL_PTR(NULL, get_output_queue(reg, target_fail.service_id, target_fail.address, target_fail.type));
    
    destroy_module(reg);

    TEST_ASSERT_EQUAL(heap_pre_init, heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    TEST_ASSERT_EQUAL_PTR(NULL, get_registry_by_name((uint8_t*) name));
    free(src_buffer_copy);
}

// Duplicate module names -----------------------------------------------
// TEST_CASE("test_duplicate_module_names", "[container_mgr]") {
//     module_registry_t *r1 = create_basic_registry("dup", NULL, 0, NULL, 0);
//     module_registry_t *r2 = create_basic_registry("dup", NULL, 0, NULL, 0);
//     TEST_ASSERT_EQUAL_PTR(r1, get_registry_by_name((uint8_t*)"dup"));
//     TEST_ASSERT_NOT_NULL(get_registry_by_handle(get_module_inst(r2)));
//     destroy_module(r1);
//     destroy_module(r2);
// }

// Mutex behaviour ------------------------------------------------------
static void* setter_thread(void *arg){
    module_registry_t *reg = arg;
    for(int i=0;i<1000;i++) set_status(reg, RUNNING);
    return NULL;
}
static void* getter_thread(void *arg){
    module_registry_t *reg = arg;
    for(int i=0;i<1000;i++) get_status(reg);
    return NULL;
}
// TEST_CASE("test_mutex_behaviour", "[container_mgr]") {
//     module_registry_t *reg = create_basic_registry("mutex", NULL, 0, NULL, 0);
//     pthread_t t1, t2;
//     pthread_create(&t1, NULL, setter_thread, reg);
//     pthread_create(&t2, NULL, getter_thread, reg);
//     pthread_join(t1, NULL);
//     pthread_join(t2, NULL);
//     TEST_ASSERT_EQUAL(RUNNING, get_status(reg));
//     destroy_module(reg);
// }




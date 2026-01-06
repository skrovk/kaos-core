#ifndef UNRELIABLE_CHANNEL_H
#define UNRELIABLE_CHANNEL_H

#include <inttypes.h>
#include <stdio.h>
#include "pthread.h"

#include "lwip/sockets.h"

#include "port.h"
#include "wasm_export.h"
#include "wamr_wrap.h"
#include "kaos_errors.h"
#include "kaos_types_shared.h"


struct module_registry_t;

kaos_error_t create_channel_queue(queue_t **queue);
kaos_error_t destroy_channel_queue(queue_t *queue);
uint32_t get(exec_env_t exec_env, service_id_t service_id, address_t container_address, char *type);
int32_t put(
    exec_env_t exec_env,
    service_id_t service_id, address_t src_addr, address_t dest_addr, char *type,
    uint8_t *content, data_size_t content_size
);

void validate_state(struct module_registry_t *to_be_destroyed);
kaos_error_t receive_container_payload(uint8_t *buffer);
kaos_error_t get_next_container_payload(message_t *msg, service_id_t *service_id);

#endif

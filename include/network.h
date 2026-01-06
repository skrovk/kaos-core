#ifndef NETWORK_H
#define NETWORK_H

#include "lwip/sockets.h"
#include "port.h"
#include "kaos_types_shared.h"
#include "container_mgr.h"


typedef struct trans_table_entry_t {
    service_id_t flow;
    address_t addr;
    struct sockaddr_in sock_addr;
} trans_table_entry_t;

typedef struct service_msg_t {
    uint8_t *buffer;
    uint32_t buffer_sz;
} service_msg_t;

typedef QueueHandle_t service_queue_t;


kaos_error_t start_server(void);
void translation_table_init(channel_t *channels, uint8_t **ip_addr, uint32_t n_entries);

// kaos_error_t send_to_log_server(service_msg_t *msg);
// kaos_error_t get_log_server_msg(service_msg_t *dest);
kaos_error_t service_put(service_msg_t *msg);
kaos_error_t notify_and_signal(module_registry_t *sig_registry, queue_t *queue);

#endif

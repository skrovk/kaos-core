#include "port.h"
#include "kaos_types_shared.h"

typedef struct trans_table_entry_t {
    service_id_t flow;
    address_t addr;
    struct sockaddr_in sock_addr;
} trans_table_entry_t;

typedef struct service_msg_t {
    uint8_t *buffer;
    uint32_t buffer_sz;
} service_msg_t;

typedef queue_t service_queue_t;

kaos_error_t service_channel_put(service_msg_t *msg);

kaos_error_t start_server(channel_t *channels, uint8_t **ip_addr, uint32_t n_entries);

#ifndef KAOS_TYPES_SHARED_H
#define KAOS_TYPES_SHARED_H

#include <inttypes.h>

#define TYPE_SZ 16

typedef uint32_t address_t;
typedef uint8_t service_id_t;
typedef uint16_t data_size_t;
typedef uint8_t kaos_signal_t;

// TODO: refactor packing/unpacking container methods
typedef struct message_t {
    // service_id_t service_id;
    address_t src_address;
    address_t dest_address;
    // char type[TYPE_SZ];
    uint32_t content_len;
    uint8_t *content;
} message_t;


// typedef struct resource_t {
//     uint8_t *import_name;
//     void *native_callback;
//     uint8_t *signature;
// } resource_t;

#endif

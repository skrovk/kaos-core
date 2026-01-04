#ifndef CBOR_COMPOSER_H
#define CBOR_COMPOSER_H

#include <inttypes.h>

#include "kaos_errors.h"
#include "kaos_monitor.h"
#include "kaos_types_shared.h"


uint32_t compose_msg_response(uint8_t **dest, uint32_t dest_size, BeaconOp op, uint8_t *module_name, kaos_error_t error);
uint32_t compose_container_msg(uint8_t **dest, uint32_t dest_size, message_t msg);

#endif
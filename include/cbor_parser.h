#ifndef CBOR_PARSER_H
#define CBOR_PARSER_H

#include <inttypes.h>
#include "container_mgr.h"
#include "kaos_errors.h"


kaos_error_t parse_monitor_message(uint8_t *buf, uint32_t buf_sz, module_config_t *module_config, interface_config_t *module_interface, uint8_t **module_name);
int parse_container_message(uint8_t *buf, uint32_t buf_sz, message_t *dest);

#endif

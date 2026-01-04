#include "cbor.h"

#include "cbor_composer.h"
#include "network.h"

// uint32_t compose_msg_response(char **dest, uint32_t dest_size, BeaconOp op, char *module_name, kaos_error_t error) {
//     CborEncoder encoder, array_encoder;

//     cbor_encoder_init(&encoder, *dest, dest_size, 0);

//     cbor_encoder_create_array(&encoder, &array_encoder, 3);
//     cbor_encode_int(&array_encoder, op);
//     cbor_encode_text_string(&array_encoder, (char *) module_name, strlen((char *) module_name));
//     cbor_encode_int(&array_encoder, error);

//     cbor_encoder_close_container(&encoder, &array_encoder);
//     uint32_t curr_buff_sz = cbor_encoder_get_buffer_size(&encoder, *dest);

//     return curr_buff_sz;
// }

// uint32_t compose_container_msg(char **dest, uint32_t dest_size, message_t msg) {
//     CborEncoder encoder, array_encoder;

//     cbor_encoder_init(&encoder, *dest, dest_size, 0);
//     cbor_encoder_create_array(&encoder, &array_encoder, 4);

//     cbor_encode_simple_value(&array_encoder, msg.service_id);
//     cbor_encode_uint(&array_encoder, msg.src_address);
//     cbor_encode_uint(&array_encoder, msg.dest_address);
//     cbor_encode_byte_string(&array_encoder, msg.content, msg.content_len);

//     uint32_t curr_buff_sz = cbor_encoder_get_buffer_size(&encoder, *dest);

//     return curr_buff_sz;
// }

// char *compose_module_status_report(module_status_report_t report) {
//     return NULL;
// }


// char *compose_kaos_status_msg(status_report_t report) {
//     return NULL;
// }

// char *compose_kaos_error_msg(char *module, char *error_string) {
//     return NULL;
// }


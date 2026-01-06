#include "cbor.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "port_logging.h"

#include "cbor_parser.h"
#include "kaos_errors.h"

const char *PARSER = "parser";

#define KAOS_MAX_RESOURCES          CONFIG_KAOS_MAX_RESOURCES
#define KAOS_MAX_CHANNELS           CONFIG_KAOS_MAX_CHANNELS
#define KAOS_MODULE_NAME_MAX_SZ     CONFIG_KAOS_MODULE_NAME_MAX_SZ
#define KAOS_SOURCE_BUFFER_MAX_SZ   CONFIG_KAOS_SOURCE_BUFFER_MAX_SZ


#define INPUTS          "INPUTS"
#define OUTPUTS         "OUTPUTS"
#define IDENTITIES      "IDENTITIES"
#define RESOURCES       "RESOURCES"
#define NAME            "NAME"
#define SOURCE_BUFFER   "SOURCE_BUFFER"
#define STACK_SIZE      "STACK_SIZE"
#define HEAP_SIZE       "HEAP_SIZE"
#define TEST            "TEST"


bool identities_configured;
bool inputs_configured;
bool outputs_configured;
bool resources_configured;


static CborError parse_byte(CborValue *value, uint8_t *dest) {
    CborError err;
    if (!cbor_value_is_simple_type(value)) {
        return CborErrorIllegalType;
    }

    err = cbor_value_get_simple_type(value, dest);
    if (err) return err;

    err = cbor_value_advance_fixed(value);
    if (err) return err;

    return CborNoError;
}


static CborError parse_int(CborValue *value, int32_t *dest) {
    CborError err;
    if (!cbor_value_is_integer(value)) {
        return CborErrorIllegalType;
    }

    int val_dest;
    err = cbor_value_get_int_checked(value, &val_dest);
    if (err) return err;

    *dest = (int32_t) val_dest;

    err = cbor_value_advance_fixed(value);
    if (err) return err;

    return CborNoError;
}


static CborError parse_uint(CborValue *value, uint32_t *dest) {
    CborError err;
    if (!cbor_value_is_unsigned_integer(value)) {
        return CborErrorIllegalType;
    }

    uint64_t val_dest;
    err = cbor_value_get_uint64(value, &val_dest);
    if (err) return err;

    *dest = (uint32_t) val_dest;

    err = cbor_value_advance_fixed(value);
    if (err) return err;

    return CborNoError;
}


static CborError parse_string(CborValue *value, uint8_t **dest, uint32_t max_sz) {
    char *buf;
    unsigned int n;
    CborError err;

    err = cbor_value_dup_text_string(value, &buf, &n, value);
    // KAOS_LOGI(__FUNCTION__, "name size: %d", n);
    if (err) return err;

    if (n > max_sz) {
        free(buf);
        return CborErrorTooManyItems;
    }

    *dest = (uint8_t *) buf;

    return CborNoError;
}


static CborError parse_channel(CborValue *value, channel_t *channel) {
    CborError err;
    if (!cbor_value_is_simple_type(value)) {
        return CborErrorIllegalType;
    }
    cbor_value_get_simple_type(value, &(channel->service_id));

    err = cbor_value_advance_fixed(value);
    if (err || cbor_value_at_end(value)) return err;

    /* address is an unsigned integer */
    err = parse_uint(value, &(channel->address));
    if (err || cbor_value_at_end(value)) return err;

    if (!cbor_value_is_text_string(value)) return CborErrorIllegalType;

    uint8_t *buf;
    size_t sz;
    cbor_value_calculate_string_length(value, &sz);
    if (sz > sizeof(channel->type)) {
        return CborErrorImproperValue;
    }
    err = parse_string(value, &buf, sizeof(channel->type));
    memcpy(&(channel->type), buf, strlen((char *) buf));
    if (err) return err;

    if (!cbor_value_at_end(value)) {
        // return ERROR;  TODO
        printf("%s: Parser error", __FUNCTION__);
    }

    return CborNoError;
}


static CborError parse_channels(CborValue *value, channel_t **channels, int16_t *n_channels) {
    CborError err;
    if (!cbor_value_is_array(value)) return CborInvalidType;

    unsigned int array_len;
    cbor_value_get_array_length(value, &array_len);
    memcpy(n_channels, (uint8_t *) (&array_len), 2);

    if (*n_channels > KAOS_MAX_CHANNELS) return CborErrorTooManyItems;

    CborValue recursed;
    err = cbor_value_enter_container(value, &recursed);
    if (err)
        return err;       // parse error

    *channels = calloc(*n_channels, sizeof(channel_t));


    for (int i = 0; i < *n_channels; i++) {
        if (!cbor_value_is_array(&recursed)) return CborErrorIllegalType;

        CborValue array_recursed;
        cbor_value_enter_container(&recursed, &array_recursed);

        err = parse_channel(&array_recursed, *channels + i);
        if (err) return err;

        err = cbor_value_leave_container(&recursed, &array_recursed);
        if (err) return err;
    }

    err = cbor_value_leave_container(value, &recursed);
    if (err)
        return err;       // parse error

    return CborNoError;

}


static CborError parse_resources(CborValue *value, export_symbol_t **resources, int16_t *n_resources) {
    CborError err;
    if (!cbor_value_is_array(value)) {
        return CborInvalidType;
    }

    unsigned int array_len;
    cbor_value_get_array_length(value, &array_len);
    memcpy(n_resources, (uint8_t *) (&array_len), 2);

    CborValue recursed;
    err = cbor_value_enter_container(value, &recursed);
    if (err)
        return err;       // parse error

    *resources = calloc(*n_resources, sizeof(export_symbol_t));

    for (int i = 0; i < *n_resources; i++) {
        if (!cbor_value_is_text_string(&recursed)) {
            return CborInvalidType;
        }

        char *buf;
        unsigned int buf_size;
        err = cbor_value_dup_text_string(&recursed, &buf, &buf_size, &recursed);
        if (err) return err;

        (*resources + i)->symbol = buf;
    }

    err = cbor_value_leave_container(value, &recursed);
    if (err)
        return err;       // parse error

    return CborNoError;
}


static CborError parse_buffer(CborValue *value, uint8_t **source_buffer, uint32_t *buffer_sz) {
    CborError err;
    if (!cbor_value_is_byte_string(value)) {
        return CborInvalidType;
    }

    size_t sz;
    err = cbor_value_calculate_string_length(value, &sz);
    if (err != CborNoError) return err;

    if (sz == 0 || sz > (size_t) KAOS_SOURCE_BUFFER_MAX_SZ) {
        KAOS_LOGE(__FUNCTION__, "Source buffer size %zu exceeds max size %d", sz, KAOS_SOURCE_BUFFER_MAX_SZ);
        return CborErrorTooManyItems;
    }

    unsigned int n;
    err = cbor_value_dup_byte_string(value, source_buffer, &n, value);
    memcpy(buffer_sz, &n, 4);
    if (err) return err;

    return CborNoError;
}


static CborError parse_iterative(CborValue *curr_item, module_config_t *module_config, interface_config_t *interface_config, uint8_t **module_name) {
    while (!cbor_value_at_end(curr_item)) {
        CborError err;
        CborType type = cbor_value_get_type(curr_item);

        switch (type) {
        case CborMapType: {
            CborValue recursed;
            if (!cbor_value_is_container(curr_item)) return CborErrorIllegalType;

            err = cbor_value_enter_container(curr_item, &recursed);
            if (err)
                return err;       // parse error

            err = parse_iterative(&recursed, module_config, interface_config, module_name);
            if (err)
                return err;       // parse error

            err = cbor_value_leave_container(curr_item, &recursed);
            if (err)
                return err;       // parse error
            break;
            }
        case CborTextStringType: {
            char *buf;
            unsigned int n;

            err = cbor_value_dup_text_string(curr_item, &buf, &n, curr_item);
            if (err) return err;

            if (strcasecmp(buf, INPUTS) == 0) {
                inputs_configured = true;
                err = parse_channels(curr_item, &(interface_config->inputs), &(interface_config->n_inputs));
                if (err) return err;
            } else if (strcasecmp(buf, OUTPUTS) == 0) {
                outputs_configured = true;
                err = parse_channels(curr_item, &(interface_config->outputs), &(interface_config->n_outputs));
                if (err) return err;
            } else if (strcasecmp(buf, IDENTITIES) == 0) {
                identities_configured = true;
                err = parse_channels(curr_item, &(interface_config->identities), &(interface_config->n_identities));
                if (err) return err;
            } else if (strcasecmp(buf, RESOURCES) == 0) {
                // KAOS_LOGE(__FUNCTION__, "Resources configured");
                resources_configured = true;
                err = parse_resources(curr_item, &(interface_config->resources), &(interface_config->n_resources));
                if (err) return err;
            } else if (strcasecmp(buf, NAME) == 0) {
                uint8_t *buf;
                err = parse_string(curr_item, &buf, KAOS_MODULE_NAME_MAX_SZ);
                if (err) return err;
                *module_name = buf;
            } else if (strcasecmp(buf, SOURCE_BUFFER) == 0) {
                err = parse_buffer(curr_item, &(module_config->source_buffer), &(module_config->source_buffer_size));
                if (err) return err;
            } else if (strcasecmp(buf, STACK_SIZE) == 0) {
                // KAOS_LOGI(PARSER, STACK_SIZE);
                err = parse_uint(curr_item, &(module_config->stack_size));
                if (err) return err;
            } else if (strcasecmp(buf, HEAP_SIZE) == 0) {
                // KAOS_LOGI(PARSER, HEAP_SIZE);
                err = parse_uint(curr_item, &(module_config->heap_size));
                if (err) return err;
            } else if (strcasecmp(buf, TEST) == 0) {
                uint8_t *buf;
                err = parse_string(curr_item, &buf, KAOS_MODULE_NAME_MAX_SZ);
                if (err) return err;
                *module_name = buf;
            } else {
                return CborErrorImproperValue;
            }
            free(buf);
            break;
        }
        default:
            return CborErrorIllegalType;
        }
    }

    return CborNoError;
}


kaos_error_t parse_monitor_message(uint8_t *buf, uint32_t buf_sz, module_config_t *module_config, interface_config_t *module_interface, uint8_t **module_name) {
    CborParser parser;
    CborValue it;
    CborError err = cbor_parser_init(buf, buf_sz, 0, &parser, &it);
    if (err) return err;

    identities_configured = false;
    inputs_configured = false;
    outputs_configured = false;
    resources_configured = false;

    err = parse_iterative(&it, module_config, module_interface, module_name);

    if (!identities_configured) {
        module_interface->n_identities = (int16_t) -1;
    }
    if (!inputs_configured) {
       module_interface->n_inputs = (int16_t) -1;
    }
    if (!outputs_configured) {
        module_interface->n_outputs = (int16_t) -1;
    }
    if (!resources_configured) module_interface->n_resources = (int16_t) -1;

    if (err) {
        KAOS_LOGE(PARSER, "CBOR parsing failure at offset %d: %s\n",
                it.source.ptr - buf, cbor_error_string(err));
        return KaosModuleConfigError;
    }
    return KaosSuccess;
}

// int parse_container_message(uint8_t *buf, uint32_t buf_sz, message_t *dest) {
//     // Num of message fields without buffer length
//     const int NUM_MSG_MEMBERS = 4;
//     CborParser parser;
//     CborValue value;

//     CborError err = cbor_parser_init(buf, buf_sz, 0, &parser, &value);

//     if (!cbor_value_is_array(&value)) {
//         return CborInvalidType;
//     }

//     unsigned int array_len;
//     cbor_value_get_array_length(&value, &array_len);

//     if (NUM_MSG_MEMBERS != 4) {
//         return CborErrorTooManyItems;
//     }

//     CborValue recursed;
//     err = cbor_value_enter_container(&value, &recursed);
//     if (err) return err;       // parse error

//     err = parse_byte(&recursed, &(dest->service_id));
//     if (err) return err;

//     err = parse_uint(&recursed, &(dest->src_address));
//     if (err) return err;

//     err = parse_uint(&recursed, &(dest->dest_address));
//     if (err) return err;

//     err = parse_buffer(&recursed, &(dest->content), &(dest->content_len));
//     if (err) return err;

//     err = cbor_value_leave_container(&value, &recursed);

//     if (!cbor_value_at_end(&value)) return CborErrorTooManyItems;

//     return CborNoError;
// }

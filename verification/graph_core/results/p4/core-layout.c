#include <stddef.h>
#include "kaos_admission.h"
#include "kaos_endpoint.h"
#include "kaos_edge.h"
#include "kaos_node.h"

#define SIZE(t) const unsigned char sz__##t[sizeof(t)] = {0}
#define FIELD(t,m) const unsigned char field__##t##__##m[sizeof(((t *)0)->m)] = {0}; \
    const unsigned char off__##t##__##m[offsetof(t,m)+1] = {0}

SIZE(core_admission);
FIELD(core_admission,node_bytes); FIELD(core_admission,byte_limit);
FIELD(core_admission,reserved_bytes); FIELD(core_admission,peak_reserved_bytes);
FIELD(core_admission,local_device); FIELD(core_admission,authority);
FIELD(core_admission,peers); FIELD(core_admission,peer_count); FIELD(core_admission,configured);
FIELD(core_admission,nodes); FIELD(core_admission,endpoints); FIELD(core_admission,records);
SIZE(core_reserved_node);
FIELD(core_reserved_node,instance); FIELD(core_reserved_node,lifetime); FIELD(core_reserved_node,prepare);
FIELD(core_reserved_node,logical_node_id); FIELD(core_reserved_node,ports); FIELD(core_reserved_node,notifications);
FIELD(core_reserved_node,port_count); FIELD(core_reserved_node,established); FIELD(core_reserved_node,occupied);
SIZE(core_reserved_endpoint);
FIELD(core_reserved_endpoint,binding); FIELD(core_reserved_endpoint,lifetime); FIELD(core_reserved_endpoint,prepare);
FIELD(core_reserved_endpoint,sender); FIELD(core_reserved_endpoint,occupied);
SIZE(core_admission_record);
FIELD(core_admission_record,id); FIELD(core_admission_record,deadline_us); FIELD(core_admission_record,node);
FIELD(core_admission_record,report); FIELD(core_admission_record,evidence); FIELD(core_admission_record,local_facts);
FIELD(core_admission_record,request); FIELD(core_admission_record,scope); FIELD(core_admission_record,request_present);
FIELD(core_admission_record,cleanup_selected); FIELD(core_admission_record,endpoint_only); FIELD(core_admission_record,occupied);
SIZE(core_request_scope);
FIELD(core_request_scope,kind); FIELD(core_request_scope,channel_count); FIELD(core_request_scope,port_count);
FIELD(core_request_scope,object_count); FIELD(core_request_scope,logical_node_id); FIELD(core_request_scope,coordinator);
FIELD(core_request_scope,instance); FIELD(core_request_scope,allowance_us); FIELD(core_request_scope,cleanup_allowance_us);
FIELD(core_request_scope,ports); FIELD(core_request_scope,bindings); FIELD(core_request_scope,predicate);
SIZE(core_node_request);
FIELD(core_node_request,logical_node_id); FIELD(core_node_request,addition_allowance_us); FIELD(core_node_request,cleanup_allowance_us);
FIELD(core_node_request,artifact_bytes); FIELD(core_node_request,artifact_crc32); FIELD(core_node_request,required_bytes);
FIELD(core_node_request,artifact_ref_size); FIELD(core_node_request,configuration_size); FIELD(core_node_request,artifact_ref);
FIELD(core_node_request,configuration);
SIZE(core_lifetime);
FIELD(core_lifetime,byte_limit); FIELD(core_lifetime,bytes); FIELD(core_lifetime,peak_bytes);
FIELD(core_lifetime,allocations); FIELD(core_lifetime,last_ticket); FIELD(core_lifetime,accesses); FIELD(core_lifetime,closing);
SIZE(core_channel_notification);
FIELD(core_channel_notification,channel); FIELD(core_channel_notification,creating_op);
FIELD(core_channel_notification,permissions); FIELD(core_channel_notification,attached);
FIELD(core_channel_notification,pending); FIELD(core_channel_notification,occupied);
SIZE(core_endpoint_service);
FIELD(core_endpoint_service,admission); FIELD(core_endpoint_service,queues); FIELD(core_endpoint_service,next_sender);
FIELD(core_endpoint_service,notify); FIELD(core_endpoint_service,context);
SIZE(core_endpoint_queue);
FIELD(core_endpoint_queue,payload); FIELD(core_endpoint_queue,sizes); FIELD(core_endpoint_queue,head);
FIELD(core_endpoint_queue,count); FIELD(core_endpoint_queue,prepared); FIELD(core_endpoint_queue,active); FIELD(core_endpoint_queue,closed);
SIZE(core_channel); SIZE(core_binding); SIZE(core_action); SIZE(core_report); SIZE(core_predicate);
SIZE(core_edge_service); SIZE(core_node_service); SIZE(core_edge_execution); SIZE(core_node_deadline);

typedef struct {
    core_op_id id; uint64_t deadline_us; core_reserved_node *node;
    core_report evidence; uint8_t local_facts[CORE_REPORT_MAX_OBJECTS];
    core_node_request request; core_request_scope scope;
    bool request_present, cleanup_selected, endpoint_only, occupied;
} projected_record_no_report;
SIZE(projected_record_no_report);

typedef struct {
    uint8_t kind, channel_count, port_count, object_count;
    uint64_t logical_node_id, coordinator; core_instance_id instance;
    uint64_t allowance_us, cleanup_allowance_us;
    core_port ports[CORE_MAX_PORTS]; core_binding bindings[1]; core_predicate predicate;
} projected_scope_one_binding;
SIZE(projected_scope_one_binding);

typedef struct {
    core_op_id id; uint64_t deadline_us; core_reserved_node *node;
    core_report evidence; uint8_t local_facts[CORE_REPORT_MAX_OBJECTS];
    projected_scope_one_binding scope;
    bool request_present, cleanup_selected, endpoint_only, occupied;
} projected_p4_edge_record;
SIZE(projected_p4_edge_record);

typedef struct {
    uint8_t payload[2][CORE_MAX_PAYLOAD_BYTES]; uint16_t sizes[2];
    uint8_t head, count; bool prepared, active, closed;
} projected_queue_two_messages;
SIZE(projected_queue_two_messages);
typedef struct {
    uint8_t payload[CORE_MAX_QUEUE_MESSAGES][64]; uint16_t sizes[CORE_MAX_QUEUE_MESSAGES];
    uint8_t head, count; bool prepared, active, closed;
} projected_queue_64_byte_payload;
SIZE(projected_queue_64_byte_payload);

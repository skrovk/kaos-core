#ifndef KAOS_ADMISSION_INTERNAL_H
#define KAOS_ADMISSION_INTERNAL_H
#include "kaos_admission.h"
core_admit_result core_admission_reserve(core_admission *pool, core_op_id id,
                                         const core_node_request *node_request,
                                         const core_request_scope *scope,
                                         uint64_t now_us, bool endpoint_only, bool cancel,
                                         const core_admission_record **out);
core_admit_result core_admission_reserve_channel(core_admission *pool, uint64_t coordinator,
                                                 core_op_id id, uint8_t index,
                                                 const core_channel *channel, bool cancel,
                                                 const core_admission_record **out);
core_admit_result core_admission_reserve_removal(core_admission *pool, uint64_t coordinator,
                                                 core_op_id id, uint8_t index,
                                                 core_instance_id target,
                                                 const core_binding *binding,
                                                 const core_admission_record **out);
#endif

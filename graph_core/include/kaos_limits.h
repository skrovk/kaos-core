#ifndef KAOS_LIMITS_H
#define KAOS_LIMITS_H

#define CORE_MAX_ARTIFACT_REF_BYTES 32

/* Statically selected profiles. Keep the small regression profile unchanged;
 * the four-node profile qualifies actual endpoint incidence and report scope. */
#ifdef CORE_FOUR_NODE_PROFILE
#define CORE_MAX_NODE_RESERVATIONS 4
#define CORE_MAX_ENDPOINT_RESERVATIONS 32
#define CORE_MAX_ADMISSION_RECORDS 24
#define CORE_MAX_REQUEST_CHANNELS 8
#define CORE_MAX_PORTS 8
#define CORE_REPORT_MAX_OBJECTS 17
#define CORE_MAX_REQUEST_BYTES 2048
#else
#define CORE_MAX_NODE_RESERVATIONS 2
#define CORE_MAX_ENDPOINT_RESERVATIONS 8
#define CORE_MAX_ADMISSION_RECORDS 4
#define CORE_MAX_REQUEST_CHANNELS 4
#define CORE_MAX_PORTS 4
#define CORE_REPORT_MAX_OBJECTS 9
#define CORE_MAX_REQUEST_BYTES 1024
#endif

#endif

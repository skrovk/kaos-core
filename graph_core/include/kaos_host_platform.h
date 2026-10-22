#ifndef KAOS_HOST_PLATFORM_H
#define KAOS_HOST_PLATFORM_H

#include "kaos_coap.h"
#include "kaos_admission.h"

typedef enum { CORE_HOST_MANAGEMENT, CORE_HOST_APPLICATION, CORE_HOST_CLASSES } core_host_class;
typedef struct {
    uint64_t sender;
    core_host_class traffic_class;
    size_t size;
    uint8_t bytes[CORE_COAP_MAX_DATAGRAM];
} core_host_packet;
typedef enum {
    CORE_HOST_WAKE,
    CORE_HOST_PACKET,
    CORE_HOST_STOP,
    CORE_HOST_ERROR,
    CORE_HOST_EMPTY,
    CORE_HOST_DROPPED
} core_host_event;

/* Mechanisms only. wait() takes an absolute monotonic microsecond deadline
 * (UINT64_MAX means no timer), observes both classes and retains earlier
 * wakeups. It returns WAKE, STOP or ERROR without receiving a packet. recv()
 * is nonblocking and consumes at most one datagram of the selected class:
 * PACKET, DROPPED (unknown sender or oversized), EMPTY, STOP or ERROR. A
 * DROPPED datagram counts against the host's receive budget. Readiness may
 * become stale; EMPTY is always allowed. send() is nonblocking and
 * borrows bytes only during the call. Attribution is configured address/ID
 * mapping, not cryptographic authentication. All callbacks are required except
 * diagnostic and channel_observation. Observers must not block or reenter the
 * host and borrow their arguments only for the call. Observation failure must
 * never affect protocol or guest results. */
typedef struct {
    void *context;
    bool (*now)(void *, uint64_t *);
    void (*lock)(void *);
    void (*unlock)(void *);
    void (*wake)(void *);
    core_host_event (*wait)(void *, uint64_t);
    core_host_event (*recv)(void *, core_host_class, core_host_packet *);
    bool (*send)(void *, core_host_class, uint64_t, const uint8_t *, size_t);
    uint64_t (*retry_us)(void *); /* initial CoAP timeout in [2,3] seconds */
    void (*diagnostic)(void *, const char *, core_op_id, size_t);
    /* Successful guest PUT/GET only, including local channels. GET means bytes
     * copied to guest memory, not application acceptance or physical execution.
     * Runs under the registry lock. direction uses CORE_CHANNEL_PUT/GET.
     * NULL disables observation. */
    void (*channel_observation)(void *, unsigned direction, core_instance_id, core_channel_id,
                                const uint8_t *, size_t);
} core_host_platform;

#endif

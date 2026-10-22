#ifndef KAOS_HOST_UDP_H
#define KAOS_HOST_UDP_H

#include "kaos_host_platform.h"
#include "kaos_admission.h"
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <netinet/in.h>
#endif
#include <pthread.h>
#include <signal.h>

typedef struct {
    uint64_t id;
    struct sockaddr_in address;     /* management; required */
    struct sockaddr_in application; /* zero port for management-only peers */
} core_host_udp_peer;

/* BSD socket adapter shared by POSIX and ESP-IDF. Address storage and OS
 * handles stay here; the host knows only device IDs and complete datagrams. */
typedef struct {
    int sockets[CORE_HOST_CLASSES], wake_fd;
    pthread_mutex_t mutex;
    core_host_udp_peer peers[CORE_MAX_PEERS + 1]; /* remote owners + O + DB */
    size_t peer_count;
    uint32_t random_state;
    volatile sig_atomic_t stopped;
    bool eventfd_registered;
    bool mutex_initialized;
} core_host_udp;

bool core_host_udp_open(core_host_udp *udp, const struct sockaddr_in *management,
                        const struct sockaddr_in *application, const core_host_udp_peer *peers,
                        size_t peer_count, core_host_platform *out);
void core_host_udp_close(core_host_udp *udp);
/* POSIX signal handler: only sets sig_atomic_t and writes the wake descriptor. */
void core_host_udp_stop(core_host_udp *udp);
/* id:management:application uses loopback; id@IPv4:management:application
 * supplies an explicit address. Omit :application for authority/collector. */
bool core_host_udp_parse_peer(const char *text, core_host_udp_peer *out);

#endif

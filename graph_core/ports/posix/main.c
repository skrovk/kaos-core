#define _POSIX_C_SOURCE 200809L
#include "../host_udp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static core_host host;
static core_host_udp udp;
static void stop(int signal_number) { (void)signal_number; core_host_udp_stop(&udp); }
static void diagnostic(void *context, const char *name, core_op_id id, size_t value)
{
    (void)context;
    char line[96];
    const int size = snprintf(line, sizeof(line), "%s %" PRIu64 " %zu\n", name, id.value, value);
    if (size > 0 && (size_t)size < sizeof(line)) (void)write(STDOUT_FILENO, line, (size_t)size);
}
static bool number(const char *text, uint64_t max, uint64_t *out)
{
    if (*text == '\0') return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value > max) return false;
    *out = value;
    return true;
}
static struct sockaddr_in address(uint16_t port)
{
    return (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
}
int main(int argc, char **argv)
{
    core_host_config config = {.node_bytes = 131072,
        .byte_limit = CORE_MAX_NODE_RESERVATIONS * 131072u + CORE_MAX_ENDPOINT_RESERVATIONS * CORE_ENDPOINT_BYTES};
    uint64_t port, authority_port;
    if (argc < 5 || argc > 4 + CORE_MAX_PEERS ||
        !number(argv[1], UINT16_MAX, &port) || !port ||
        !number(argv[2], UINT16_MAX, &authority_port) || !authority_port || port == authority_port ||
        !number(argv[3], UINT64_MAX, &config.local_device) ||
        !number(argv[4], UINT64_MAX, &config.authority)) {
        fprintf(stderr, "usage: %s listen_port authority_port local_device_id authority_id [peer_id:udp_port ...]\n", argv[0]);
        return 2;
    }
    config.peer_count = (size_t)argc - 4;
    config.peers[0] = config.local_device;
    core_host_udp_peer routes[CORE_MAX_PEERS + 1] = {
        {.id = config.authority, .address = address((uint16_t)authority_port)}};
    size_t route_count = 1;
    for (size_t i = 1; i < config.peer_count; ++i) {
        if (!core_host_udp_parse_peer(argv[i + 4], &routes[route_count]) ||
            (routes[route_count].address.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
             routes[route_count].address.sin_port == htons((uint16_t)port))) return 2;
        config.peers[i] = routes[route_count++].id;
    }
    const char *collector = getenv("KAOS_COLLECTOR");
    if (collector != NULL) {
        if (!core_host_udp_parse_peer(collector, &routes[route_count]) ||
            (routes[route_count].address.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
             routes[route_count].address.sin_port == htons((uint16_t)port))) return 2;
        config.collector_enabled = true;
        config.collector = routes[route_count++].id;
    }
    const struct sockaddr_in local = address((uint16_t)port);
    core_host_platform platform;
    if (!core_host_udp_open(&udp, &local, routes, route_count, &platform)) return 1;
    const int flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) != 0) {
        core_host_udp_close(&udp);
        return 1;
    }
    platform.diagnostic = diagnostic;
    const struct sigaction action = {.sa_handler = stop};
    if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
        !core_host_init(&host, &config, &platform)) {
        core_host_udp_close(&udp);
        return 1;
    }
    char ready[64];
    const int size = snprintf(ready, sizeof(ready), "LISTENING %" PRIu64 "\n", port);
    (void)write(STDOUT_FILENO, ready, (size_t)size);
    const bool good = core_host_run(&host);
    core_host_destroy(&host);
    core_host_udp_close(&udp);
    return good ? 0 : 1;
}

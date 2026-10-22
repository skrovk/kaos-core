#define _POSIX_C_SOURCE 200809L
#include "host_udp.h"
#include "kaos_host.h"
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
static uint64_t observed, observation_drops;
static void stop(int signal_number)
{
    (void)signal_number;
    core_host_udp_stop(&udp);
}

static void diagnostic(void *context, const char *name, core_op_id id, size_t value)
{
    (void)context;
    char line[96];
    const int size = snprintf(line, sizeof(line), "%s %" PRIu64 " %zu\n", name, id.value, value);
    if (size > 0 && (size_t)size < sizeof(line))
        (void)write(STDOUT_FILENO, line, (size_t)size);
}

/* Opt-in host instrumentation. A bounded line fits Linux PIPE_BUF and is
 * written once to nonblocking stdout: no queue, allocation or retry under the
 * registry lock. Sequence gaps and the final counters expose observation loss.
 * Host processes share CLOCK_MONOTONIC; these times are not guest data clocks. */
static void channel_observation(void *context, unsigned direction, core_instance_id instance,
                                core_channel_id channel, const uint8_t *data, size_t size)
{
    (void)context;
    const uint64_t sequence = ++observed;
    uint64_t time;
    if (size > CORE_MAX_PAYLOAD_BYTES || !core_clock_now(&time)) {
        ++observation_drops;
        return;
    }
    char line[256 + 2 * CORE_MAX_PAYLOAD_BYTES];
    const int prefix = snprintf(
        line, sizeof(line),
        "CHANNEL %" PRIu64 " %" PRIu64 " %" PRIu64 " %s %" PRIu64 " %" PRIu64 " %" PRIu64 " ",
        sequence, time, instance.value, direction == CORE_CHANNEL_PUT ? "PUT" : "GET",
        channel.source, channel.destination, channel.service);
    if (prefix < 0 || (size_t)prefix + 2 * size + 1 > sizeof(line)) {
        ++observation_drops;
        return;
    }
    static const char hex[] = "0123456789abcdef";
    size_t length = (size_t)prefix;
    for (size_t i = 0; i < size; ++i) {
        line[length++] = hex[data[i] >> 4];
        line[length++] = hex[data[i] & 15];
    }
    line[length++] = '\n';
    if (write(STDOUT_FILENO, line, length) != (ssize_t)length)
        ++observation_drops;
}

static void transport_summary(const core_host_stats *stats)
{
    /* One bounded, nonblocking observation after workers join. A missing line
     * means the summary was unavailable; these counters never affect service. */
    char line[768];
    const int size = snprintf(
        line, sizeof(line),
        "TRANSPORT rounds=%" PRIu64 " rx_app=%" PRIu64 " rx_mgmt=%" PRIu64 " drop_app=%" PRIu64
        " drop_mgmt=%" PRIu64 " invalid_app=%" PRIu64 " invalid_mgmt=%" PRIu64 " sent_app=%" PRIu64
        " sent_mgmt=%" PRIu64 " send_fail_app=%" PRIu64 " send_fail_mgmt=%" PRIu64
        " endpoint_drops=%" PRIu64 " artifact_requests=%" PRIu64 " max_round_us=%" PRIu64
        " max_management_gap_us=%" PRIu64 " artifact_receive_max_us=%" PRIu64
        " artifact_complete_max_us=%" PRIu64 "\n",
        stats->rounds, stats->received[CORE_HOST_APPLICATION],
        stats->received[CORE_HOST_MANAGEMENT], stats->receive_drops[CORE_HOST_APPLICATION],
        stats->receive_drops[CORE_HOST_MANAGEMENT], stats->invalid_packets[CORE_HOST_APPLICATION],
        stats->invalid_packets[CORE_HOST_MANAGEMENT], stats->sent[CORE_HOST_APPLICATION],
        stats->sent[CORE_HOST_MANAGEMENT], stats->send_failures[CORE_HOST_APPLICATION],
        stats->send_failures[CORE_HOST_MANAGEMENT], stats->endpoint_drops, stats->artifact_requests,
        stats->max_round_us, stats->max_management_gap_us, stats->artifact_receive_max_us,
        stats->artifact_complete_max_us);
    if (size > 0 && (size_t)size < sizeof(line))
        (void)write(STDOUT_FILENO, line, (size_t)size);
}

static bool number(const char *text, uint64_t max, uint64_t *out)
{
    if (*text == '\0')
        return false;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9')
            return false;
    errno = 0;
    char *end;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value > max)
        return false;
    *out = value;
    return true;
}

static struct sockaddr_in address(uint16_t port)
{
    return (struct sockaddr_in){.sin_family = AF_INET,
                                .sin_port = htons(port),
                                .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
}

static bool local_route(const core_host_udp_peer *peer, uint16_t management, uint16_t application)
{
    return (peer->address.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
            (peer->address.sin_port == htons(management) ||
             peer->address.sin_port == htons(application))) ||
           (peer->application.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
            (peer->application.sin_port == htons(management) ||
             peer->application.sin_port == htons(application)));
}

int main(int argc, char **argv)
{
    core_host_config config = {.node_bytes = 131072,
                               .artifact_interval_us = CORE_HOST_ARTIFACT_INTERVAL_US,
                               .byte_limit = CORE_MAX_NODE_RESERVATIONS * 131072u +
                                             CORE_MAX_ENDPOINT_RESERVATIONS * CORE_ENDPOINT_BYTES};
    uint64_t management_port, application_port, authority_port;
    if (argc < 6 || argc > 5 + CORE_MAX_PEERS || !number(argv[1], UINT16_MAX, &management_port) ||
        !management_port || !number(argv[2], UINT16_MAX, &application_port) || !application_port ||
        management_port == application_port || !number(argv[3], UINT16_MAX, &authority_port) ||
        !authority_port || management_port == authority_port ||
        application_port == authority_port || !number(argv[4], UINT64_MAX, &config.local_device) ||
        !number(argv[5], UINT64_MAX, &config.authority)) {
        fprintf(
            stderr,
            "usage: %s management_port application_port authority_port local_device_id authority_id "
            "[peer_id:management_port:application_port ...]\n"
            "KaOS requires distinct explicit management and application ports.\n",
            argv[0]);
        return 2;
    }

    config.peer_count = (size_t)argc - 5;
    config.peers[0] = config.local_device;
    core_host_udp_peer routes[CORE_MAX_PEERS + 1] = {
        {.id = config.authority, .address = address((uint16_t)authority_port)}};
    size_t route_count = 1;
    for (size_t i = 1; i < config.peer_count; ++i) {
        if (!core_host_udp_parse_peer(argv[i + 5], &routes[route_count]) ||
            routes[route_count].application.sin_port == 0 ||
            local_route(&routes[route_count], (uint16_t)management_port,
                        (uint16_t)application_port)) {
            fputs(
                "Invalid remote owner: provide id:management:application or id@IPv4:management:application "
                "with nonlocal ports.\n",
                stderr);
            return 2;
        }
        config.peers[i] = routes[route_count++].id;
    }

    const char *collector = getenv("KAOS_COLLECTOR");
    if (collector != NULL) {
        if (!core_host_udp_parse_peer(collector, &routes[route_count]) ||
            routes[route_count].application.sin_port != 0 ||
            local_route(&routes[route_count], (uint16_t)management_port,
                        (uint16_t)application_port)) {
            fputs("Invalid collector: provide id@IPv4:management_port with a nonlocal port.\n",
                  stderr);
            return 2;
        }
        config.collector_enabled = true;
        config.collector = routes[route_count++].id;
    }

    const struct sockaddr_in management = address((uint16_t)management_port);
    const struct sockaddr_in application = address((uint16_t)application_port);
    core_host_platform platform;
    if (!core_host_udp_open(&udp, &management, &application, routes, route_count, &platform)) {
        fputs(
            "KaOS socket initialization failed: check port availability and unique peer routes.\n",
            stderr);
        return 1;
    }

    const int flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) != 0) {
        core_host_udp_close(&udp);
        return 1;
    }

    platform.diagnostic = diagnostic;
    const char *trace = getenv("KAOS_TRACE_CHANNELS");
    const bool tracing = trace != NULL && strcmp(trace, "1") == 0;
    if (tracing)
        platform.channel_observation = channel_observation;

    const struct sigaction action = {.sa_handler = stop};
    if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
        !core_host_init(&host, &config, &platform)) {
        core_host_udp_close(&udp);
        return 1;
    }
    char ready[64];
    const int size = snprintf(ready, sizeof(ready), "LISTENING %" PRIu64 " %" PRIu64 "\n",
                              management_port, application_port);
    (void)write(STDOUT_FILENO, ready, (size_t)size);

    const bool good = core_host_run(&host);
    core_host_destroy(&host);
    transport_summary(&host.stats);
    if (tracing) {
        /* Workers have joined. A missing final line makes the trace incomplete. */
        char final[96];
        const int count = snprintf(final, sizeof(final), "CHANNEL_END %" PRIu64 " %" PRIu64 "\n",
                                   observed, observation_drops);
        if (count > 0 && (size_t)count < sizeof(final))
            (void)write(STDOUT_FILENO, final, (size_t)count);
    }
    core_host_udp_close(&udp);
    return good ? 0 : 1;
}

#define _POSIX_C_SOURCE 200809L
#include "host_udp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_vfs_eventfd.h"
#else
#include <sys/eventfd.h>
#endif

static bool clock_now(void *context, uint64_t *now)
{
    (void)context;
    return core_clock_now(now);
}

static void registry_lock(void *context)
{
    pthread_mutex_lock(&((core_host_udp *)context)->mutex);
}

static void registry_unlock(void *context)
{
    pthread_mutex_unlock(&((core_host_udp *)context)->mutex);
}

static void notify(void *context)
{
    core_host_udp *udp = context;
    const uint64_t one = 1;
    const int saved_errno = errno;
    ssize_t result;
    do {
        result = write(udp->wake_fd, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
    /* A saturated counter already retains a wake. Mailboxes retain evidence. */
    errno = saved_errno;
}

static const struct sockaddr_in *peer_address(const core_host_udp_peer *peer,
                                              core_host_class traffic_class)
{
    return traffic_class == CORE_HOST_MANAGEMENT ? &peer->address : &peer->application;
}

static bool send_packet(void *context, core_host_class traffic_class, uint64_t peer,
                        const uint8_t *bytes, size_t size)
{
    core_host_udp *udp = context;
    if ((unsigned)traffic_class >= CORE_HOST_CLASSES || size > CORE_COAP_MAX_DATAGRAM)
        return false;
    for (size_t i = 0; i < udp->peer_count; ++i) {
        if (udp->peers[i].id != peer)
            continue;
        const struct sockaddr_in *address = peer_address(&udp->peers[i], traffic_class);
        if (address->sin_port == 0)
            return false;
        return sendto(udp->sockets[traffic_class], bytes, size, 0, (const struct sockaddr *)address,
                      sizeof(*address)) == (ssize_t)size;
    }
    return false;
}

static uint64_t retry_delay(void *context)
{
    core_host_udp *udp = context;
    uint32_t x = udp->random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    udp->random_state = x;
    return CORE_COAP_ACK_TIMEOUT_US + x % 1000001u;
}

static core_host_event wait_input(void *context, uint64_t deadline)
{
    core_host_udp *udp = context;
    if (udp->stopped)
        return CORE_HOST_STOP;
    struct timeval timeout, *limit = NULL;
    if (deadline != UINT64_MAX) {
        uint64_t now;
        if (!core_clock_now(&now))
            return CORE_HOST_ERROR;
        const uint64_t remaining = deadline > now ? deadline - now : 0;
        timeout.tv_sec = remaining / 1000000 > INT_MAX ? INT_MAX : (long)(remaining / 1000000);
        timeout.tv_usec = (long)(remaining % 1000000);
        limit = &timeout;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(udp->wake_fd, &read_fds);
    int max_fd = udp->wake_fd;

    for (size_t i = 0; i < CORE_HOST_CLASSES; ++i) {
        FD_SET(udp->sockets[i], &read_fds);
        if (udp->sockets[i] > max_fd)
            max_fd = udp->sockets[i];
    }
    const int ready = select(max_fd + 1, &read_fds, NULL, NULL, limit);
    if (udp->stopped)
        return CORE_HOST_STOP;
    if (ready < 0)
        return errno == EINTR ? CORE_HOST_WAKE : CORE_HOST_ERROR;
    if (ready == 0)
        return CORE_HOST_WAKE;
    if (FD_ISSET(udp->wake_fd, &read_fds)) {
        uint64_t count;
        (void)read(udp->wake_fd, &count, sizeof(count));
    }
    return CORE_HOST_WAKE;
}

static core_host_event receive_packet(void *context, core_host_class traffic_class,
                                      core_host_packet *packet)
{
    core_host_udp *udp = context;
    if (udp->stopped)
        return CORE_HOST_STOP;
    if ((unsigned)traffic_class >= CORE_HOST_CLASSES)
        return CORE_HOST_ERROR;

    /* One extra byte detects oversized/truncated packets on both BSD stacks. */
    uint8_t bytes[CORE_COAP_MAX_DATAGRAM + 1];
    struct sockaddr_in sender;
    socklen_t address_size = sizeof(sender);
    const ssize_t size = recvfrom(udp->sockets[traffic_class], bytes, sizeof(bytes), 0,
                                  (struct sockaddr *)&sender, &address_size);
    if (size < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? CORE_HOST_EMPTY
                                                                         : CORE_HOST_ERROR;
    if ((size_t)size > sizeof(packet->bytes) || address_size != sizeof(sender) ||
        sender.sin_family != AF_INET)
        return CORE_HOST_DROPPED;

    for (size_t i = 0; i < udp->peer_count; ++i) {
        const struct sockaddr_in *address = peer_address(&udp->peers[i], traffic_class);
        if (sender.sin_addr.s_addr != address->sin_addr.s_addr ||
            sender.sin_port != address->sin_port)
            continue;

        packet->sender = udp->peers[i].id;
        packet->traffic_class = traffic_class;
        packet->size = (size_t)size;
        memcpy(packet->bytes, bytes, packet->size);
        return CORE_HOST_PACKET;
    }
    return CORE_HOST_DROPPED;
}

static bool same_address(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_port != 0 && a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

bool core_host_udp_open(core_host_udp *udp, const struct sockaddr_in *management,
                        const struct sockaddr_in *application, const core_host_udp_peer *peers,
                        size_t peer_count, core_host_platform *out)
{
    *udp = (core_host_udp){.sockets = {-1, -1}, .wake_fd = -1};
    if (peer_count > CORE_MAX_PEERS + 1 || management->sin_family != AF_INET ||
        application->sin_family != AF_INET ||
        (management->sin_port != 0 && management->sin_port == application->sin_port))
        return false;

    for (size_t i = 0; i < peer_count; ++i) {
        if (peers[i].address.sin_family != AF_INET || peers[i].address.sin_port == 0 ||
            (peers[i].application.sin_port != 0 &&
             (peers[i].application.sin_family != AF_INET ||
              same_address(&peers[i].address, &peers[i].application))))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (peers[i].id == peers[j].id || same_address(&peers[i].address, &peers[j].address) ||
                same_address(&peers[i].application, &peers[j].application))
                return false;
    }

    if (pthread_mutex_init(&udp->mutex, NULL) != 0)
        return false;
    udp->mutex_initialized = true;
    memcpy(udp->peers, peers, peer_count * sizeof(*peers));
    udp->peer_count = peer_count;

    uint64_t time;
    if (!core_clock_now(&time))
        goto fail;
    udp->random_state = (uint32_t)time | 1u;
#ifdef ESP_PLATFORM
    const esp_vfs_eventfd_config_t config = {.max_fds = 1};
    if (esp_vfs_eventfd_register(&config) != ESP_OK)
        goto fail;
    udp->eventfd_registered = true;
    udp->wake_fd = eventfd(0, 0);
#else
    udp->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
    if (udp->wake_fd < 0 || udp->wake_fd >= FD_SETSIZE)
        goto fail;

    const struct sockaddr_in *local[CORE_HOST_CLASSES] = {management, application};
    for (size_t i = 0; i < CORE_HOST_CLASSES; ++i) {
        udp->sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp->sockets[i] < 0 || udp->sockets[i] >= FD_SETSIZE)
            goto fail;
        const int flags = fcntl(udp->sockets[i], F_GETFL, 0);
        if (flags < 0 || fcntl(udp->sockets[i], F_SETFL, flags | O_NONBLOCK) != 0 ||
            bind(udp->sockets[i], (const struct sockaddr *)local[i], sizeof(*local[i])) != 0)
            goto fail;
    }

    *out = (core_host_platform){.context = udp,
                                .now = clock_now,
                                .lock = registry_lock,
                                .unlock = registry_unlock,
                                .wake = notify,
                                .wait = wait_input,
                                .recv = receive_packet,
                                .send = send_packet,
                                .retry_us = retry_delay};
    return true;
fail:
    core_host_udp_close(udp);
    return false;
}

void core_host_udp_stop(core_host_udp *udp)
{
    udp->stopped = 1;
    notify(udp);
}

void core_host_udp_close(core_host_udp *udp)
{
    for (size_t i = 0; i < CORE_HOST_CLASSES; ++i) {
        if (udp->sockets[i] >= 0)
            close(udp->sockets[i]);
        udp->sockets[i] = -1;
    }
    if (udp->wake_fd >= 0)
        close(udp->wake_fd);
    udp->wake_fd = -1;
#ifdef ESP_PLATFORM
    if (udp->eventfd_registered)
        (void)esp_vfs_eventfd_unregister();
    udp->eventfd_registered = false;
#endif
    if (udp->mutex_initialized)
        pthread_mutex_destroy(&udp->mutex);
    udp->mutex_initialized = false;
}

/* Parse one port and leave its optional separator for the caller. */
static bool parse_port(const char *text, char separator, uint16_t *port, const char **end)
{
    const char *p = text;
    while (*p >= '0' && *p <= '9')
        ++p;
    if (p == text || (*p != '\0' && *p != separator))
        return false;
    errno = 0;
    char *parsed_end;
    const unsigned long value = strtoul(text, &parsed_end, 10);
    if (errno || parsed_end != p || value == 0 || value > UINT16_MAX)
        return false;
    *port = (uint16_t)value;
    *end = p;
    return true;
}

bool core_host_udp_parse_peer(const char *text, core_host_udp_peer *out)
{
    if (text == NULL || *text < '0' || *text > '9')
        return false;
    const char *p = text;
    while (*p >= '0' && *p <= '9')
        ++p;
    if (*p != ':' && *p != '@')
        return false;
    errno = 0;
    char *end;
    const uint64_t id = strtoull(text, &end, 10);
    if (errno || end != p)
        return false;
    struct in_addr address = {.s_addr = htonl(INADDR_LOOPBACK)};
    if (*p == '@') {
        const char *colon = strchr(++p, ':');
        char ip[INET_ADDRSTRLEN];
        if (colon == NULL || colon == p || (size_t)(colon - p) >= sizeof(ip))
            return false;
        memcpy(ip, p, (size_t)(colon - p));
        ip[colon - p] = '\0';
        if (inet_pton(AF_INET, ip, &address) != 1)
            return false;
        p = colon;
    }
    ++p;
    const char *q;
    uint16_t port;
    if (!parse_port(p, ':', &port, &q))
        return false;
    *out = (core_host_udp_peer){
        .id = id, .address = {.sin_family = AF_INET, .sin_addr = address, .sin_port = htons(port)}};
    if (*q == ':') {
        uint16_t app_port;
        if (!parse_port(q + 1, '\0', &app_port, &q) || app_port == port)
            return false;
        out->application = (struct sockaddr_in){
            .sin_family = AF_INET, .sin_addr = address, .sin_port = htons(app_port)};
    }
    return true;
}

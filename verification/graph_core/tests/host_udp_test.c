#include "admission_fixture.h"
#include "host_udp.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t deadline(void)
{
    uint64_t now;
    CHECK(core_clock_now(&now));
    return now + 500000;
}
static int bind_sender(struct sockaddr_in *address)
{
    const int sender = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(sender >= 0);
    *address = (struct sockaddr_in){.sin_family = AF_INET,
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    CHECK(bind(sender, (const struct sockaddr *)address, sizeof(*address)) == 0);
    socklen_t size = sizeof(*address);
    CHECK(getsockname(sender, (struct sockaddr *)address, &size) == 0);
    /* Broken routing must fail the test instead of hanging indefinitely. */
    const struct timeval timeout = {.tv_sec = 1};
    CHECK(setsockopt(sender, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    return sender;
}
int main(void)
{
    core_host_udp_peer peer;
    CHECK(core_host_udp_parse_peer("0@127.0.0.1:1234", &peer));
    CHECK(peer.id == 0 && ntohs(peer.address.sin_port) == 1234);
    CHECK(core_host_udp_parse_peer("18446744073709551615:65535", &peer));
    CHECK(peer.id == UINT64_MAX);
    CHECK(!core_host_udp_parse_peer("18446744073709551616:1234", &peer));
    CHECK(!core_host_udp_parse_peer("1@invalid:1234", &peer));
    CHECK(!core_host_udp_parse_peer("1:+1234", &peer));
    CHECK(!core_host_udp_parse_peer("1:65536", &peer));
    CHECK(core_host_udp_parse_peer("1@127.0.0.1:1234:5678", &peer));
    CHECK(ntohs(peer.address.sin_port) == 1234 && ntohs(peer.application.sin_port) == 5678);
    CHECK(peer.address.sin_addr.s_addr == peer.application.sin_addr.s_addr);
    CHECK(core_host_udp_parse_peer("1:1234:5678", &peer));
    CHECK(!core_host_udp_parse_peer("1:1234:1234", &peer));
    CHECK(!core_host_udp_parse_peer("1:1234:0", &peer));
    CHECK(!core_host_udp_parse_peer("1:1234:+5678", &peer));
    CHECK(!core_host_udp_parse_peer("1:1234:65536", &peer));
    CHECK(!core_host_udp_parse_peer("1:1234:", &peer));
    CHECK(!core_host_udp_parse_peer("1:1234:5678:9", &peer));
    peer = (core_host_udp_peer){.id = 99};
    const int sender = bind_sender(&peer.address);
    const int application_sender = bind_sender(&peer.application);
    core_host_udp_peer management_only = {.id = 100};
    const int authority_sender = bind_sender(&management_only.address);
    const core_host_udp_peer peers[] = {peer, management_only};
    socklen_t size = sizeof(peer.address);
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    struct sockaddr_in application = local;
    core_host_udp udp;
    core_host_platform platform;
    CHECK(core_host_udp_open(&udp, &local, &application, peers, 2, &platform));
    CHECK(getsockname(udp.sockets[CORE_HOST_MANAGEMENT], (struct sockaddr *)&local, &size) == 0);
    CHECK(getsockname(udp.sockets[CORE_HOST_APPLICATION], (struct sockaddr *)&application, &size) == 0);
    CHECK(local.sin_port != application.sin_port);
    core_host_packet packet;
    platform.wake(platform.context); /* issued before entering the wait */
    CHECK(platform.wait(platform.context, UINT64_MAX) == CORE_HOST_WAKE);
    CHECK(platform.recv(platform.context, CORE_HOST_MANAGEMENT, &packet) == CORE_HOST_EMPTY);
    CHECK(platform.recv(platform.context, CORE_HOST_APPLICATION, &packet) == CORE_HOST_EMPTY);
    uint8_t bytes[CORE_COAP_MAX_DATAGRAM + 32] = {1, 2, 3};
    CHECK(sendto(sender, bytes, sizeof(bytes), 0, (struct sockaddr *)&local, sizeof(local)) == sizeof(bytes));
    CHECK(platform.wait(platform.context, deadline()) == CORE_HOST_WAKE);
    CHECK(platform.recv(platform.context, CORE_HOST_MANAGEMENT, &packet) == CORE_HOST_DROPPED);
    CHECK(sendto(sender, bytes, 3, 0, (struct sockaddr *)&local, sizeof(local)) == 3);
    CHECK(platform.wait(platform.context, deadline()) == CORE_HOST_WAKE);
    CHECK(platform.recv(platform.context, CORE_HOST_MANAGEMENT, &packet) == CORE_HOST_PACKET);
    CHECK(packet.sender == 99 && packet.size == 3 && memcmp(packet.bytes, bytes, 3) == 0);
    CHECK(packet.traffic_class == CORE_HOST_MANAGEMENT);
    struct sockaddr_in source;
    CHECK(platform.send(platform.context, CORE_HOST_MANAGEMENT, 99, bytes, 3));
    CHECK(recvfrom(sender, bytes, sizeof(bytes), 0, (struct sockaddr *)&source, &size) == 3);
    CHECK(source.sin_port == local.sin_port);
    CHECK(!platform.send(platform.context, CORE_HOST_MANAGEMENT, 123, bytes, 3));
    CHECK(platform.send(platform.context, CORE_HOST_APPLICATION, 99, bytes, 3));
    CHECK(recvfrom(application_sender, bytes, sizeof(bytes), 0, (struct sockaddr *)&source, &size) == 3);
    CHECK(source.sin_port == application.sin_port);
    CHECK(!platform.send(platform.context, CORE_HOST_APPLICATION, 100, bytes, 3));
    CHECK(platform.send(platform.context, CORE_HOST_MANAGEMENT, 100, bytes, 3));
    CHECK(recv(authority_sender, bytes, sizeof(bytes), 0) == 3);

    /* Both queues can remain ready; the caller chooses which one to service. */
    CHECK(sendto(sender, bytes, 3, 0, (struct sockaddr *)&local, sizeof(local)) == 3);
    CHECK(sendto(application_sender, bytes, 3, 0, (struct sockaddr *)&application, sizeof(application)) == 3);
    CHECK(platform.wait(platform.context, deadline()) == CORE_HOST_WAKE);
    CHECK(platform.recv(platform.context, CORE_HOST_APPLICATION, &packet) == CORE_HOST_PACKET);
    CHECK(packet.sender == 99 && packet.traffic_class == CORE_HOST_APPLICATION);
    CHECK(platform.recv(platform.context, CORE_HOST_APPLICATION, &packet) == CORE_HOST_EMPTY);
    CHECK(platform.recv(platform.context, CORE_HOST_MANAGEMENT, &packet) == CORE_HOST_PACKET);

    /* A wrong-class source consumes exactly one budget item; it cannot drain
     * past the valid packet queued behind it or borrow another class's route. */
    CHECK(sendto(sender, bytes, 3, 0, (struct sockaddr *)&application, sizeof(application)) == 3);
    CHECK(sendto(application_sender, bytes, 3, 0, (struct sockaddr *)&application, sizeof(application)) == 3);
    CHECK(platform.recv(platform.context, CORE_HOST_APPLICATION, &packet) == CORE_HOST_DROPPED);
    CHECK(platform.recv(platform.context, CORE_HOST_APPLICATION, &packet) == CORE_HOST_PACKET);
    CHECK(platform.recv(platform.context, CORE_HOST_APPLICATION, &packet) == CORE_HOST_EMPTY);
    CHECK(sendto(application_sender, bytes, sizeof(bytes), 0,
                 (struct sockaddr *)&application, sizeof(application)) == sizeof(bytes));
    CHECK(platform.recv(platform.context, CORE_HOST_APPLICATION, &packet) == CORE_HOST_DROPPED);
    core_host_udp_stop(&udp);
    CHECK(platform.wait(platform.context, UINT64_MAX) == CORE_HOST_STOP);
    CHECK(platform.recv(platform.context, CORE_HOST_APPLICATION, &packet) == CORE_HOST_STOP);
    core_host_udp_close(&udp);

    /* Reject ambiguous attribution independently for each class. */
    core_host_udp_peer conflicts[] = {peer, peer};
    conflicts[1].id = 101;
    CHECK(!core_host_udp_open(&udp, &local, &application, conflicts, 2, &platform));
    conflicts[1].address = management_only.address;
    CHECK(!core_host_udp_open(&udp, &local, &application, conflicts, 2, &platform));
    CHECK(!core_host_udp_open(&udp, &local, &local, &peer, 1, &platform));

    /* Occupied second port fails after the first bind. Both descriptors and
     * the wake descriptor close, and the first port is immediately reusable. */
    CHECK(!core_host_udp_open(&udp, &local, &peer.application, &peer, 1, &platform));
    CHECK(udp.sockets[CORE_HOST_MANAGEMENT] == -1 && udp.sockets[CORE_HOST_APPLICATION] == -1);
    CHECK(udp.wake_fd == -1 && !udp.mutex_initialized);
    CHECK(core_host_udp_open(&udp, &local, &application, &peer, 1, &platform));
    core_host_udp_close(&udp);
    close(sender);
    close(application_sender);
    close(authority_sender);
    puts("UDP adapter: two-class routes, bounded receive, retained wake, partial startup and stop passed");
}

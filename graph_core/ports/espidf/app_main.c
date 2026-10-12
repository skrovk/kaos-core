#include "host_udp.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static core_host host;
static core_host_udp udp;

void app_main(void)
{
    /* Network configuration belongs to the board entrypoint. No graph state
     * exists until networking is ready; reconnect is not process recovery. */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());
    vTaskPrioritySet(NULL, 3);

    core_host_config config = {.local_device = CONFIG_KAOS_DEVICE_ID, .peer_count = 1,
        .peers = {CONFIG_KAOS_DEVICE_ID}, .node_bytes = CONFIG_KAOS_NODE_BYTES,
        .byte_limit = CORE_MAX_NODE_RESERVATIONS * (size_t)CONFIG_KAOS_NODE_BYTES +
                      CORE_MAX_ENDPOINT_RESERVATIONS * CORE_ENDPOINT_BYTES};
    core_host_udp_peer routes[CORE_MAX_PEERS + 1];
    if (!core_host_udp_parse_peer(CONFIG_KAOS_AUTHORITY, &routes[0])) {
        puts("Invalid KaOS authority");
        return;
    }
    config.authority = routes[0].id;
    size_t route_count = 1;
    char peers[] = CONFIG_KAOS_PEERS;
    char *saved;
    for (char *peer = strtok_r(peers, ",", &saved); peer; peer = strtok_r(NULL, ",", &saved)) {
        if (config.peer_count == CORE_MAX_PEERS || !core_host_udp_parse_peer(peer, &routes[route_count])) {
            puts("Invalid KaOS peer enrollment");
            return;
        }
        config.peers[config.peer_count++] = routes[route_count++].id;
    }
    if (CONFIG_KAOS_COLLECTOR[0]) {
        if (!core_host_udp_parse_peer(CONFIG_KAOS_COLLECTOR, &routes[route_count])) {
            puts("Invalid KaOS collector");
            return;
        }
        config.collector_enabled = true;
        config.collector = routes[route_count++].id;
    }
    const struct sockaddr_in local = {.sin_family = AF_INET,
        .sin_port = htons(CONFIG_KAOS_UDP_PORT), .sin_addr = {.s_addr = htonl(INADDR_ANY)}};
    core_host_platform platform;
    if (!core_host_udp_open(&udp, &local, routes, route_count, &platform)) {
        puts("KaOS socket initialization failed");
        return;
    }
    if (core_host_init(&host, &config, &platform)) {
        printf("LISTENING %d\n", CONFIG_KAOS_UDP_PORT);
        (void)core_host_run(&host);
        core_host_destroy(&host);
    } else puts("KaOS host initialization failed");
    core_host_udp_close(&udp);
}

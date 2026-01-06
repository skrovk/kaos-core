/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <netdb.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "port_logging.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "esp_heap_caps.h"

// #include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "kaos_errors.h"
#include "unreliable_channel.h"
#include "kaos_types_shared.h"
#include "kaos_monitor.h"

#include "network.h"

#define KAOS_WIFI_SSID             CONFIG_KAOS_WIFI_SSID
#define KAOS_WIFI_PASS             CONFIG_KAOS_WIFI_PASSWORD
#define KAOS_MAXIMUM_RETRY         CONFIG_KAOS_MAXIMUM_RETRY
#define KAOS_STATIC_NETMASK_ADDR   CONFIG_KAOS_STATIC_NETMASK_ADDR
#define KAOS_STATIC_GW_ADDR        CONFIG_KAOS_STATIC_GW_ADDR

#define KAOS_SERVICE_IP_ADDR       CONFIG_KAOS_SERVICE_IP_ADDR
#define KAOS_STATIC_IP_SELF   CONFIG_KAOS_STATIC_IP_SELF

#ifdef CONFIG_KAOS_STATIC_DNS_AUTO
#define KAOS_MAIN_DNS_SERVER       KAOS_STATIC_GW_ADDR
#define KAOS_BACKUP_DNS_SERVER     "0.0.0.0"
#else
#define KAOS_MAIN_DNS_SERVER       CONFIG_KAOS_STATIC_DNS_SERVER_MAIN
#define KAOS_BACKUP_DNS_SERVER     CONFIG_KAOS_STATIC_DNS_SERVER_BACKUP
#endif
#ifdef CONFIG_KAOS_STATIC_DNS_RESOLVE_TEST
#define KAOS_RESOLVE_DOMAIN        CONFIG_KAOS_STATIC_RESOLVE_DOMAIN
#endif

#define KAOS_SOURCE_BUFFER_MAX_SZ   CONFIG_KAOS_SOURCE_BUFFER_MAX_SZ

#define PORT CONFIG_KAOS_PORT
#define SERVICE_PORT CONFIG_KAOS_SERVICE_PORT
#define MAX_SERVICE_QUEUE_ITEMS 20

#define INIT_RX_BUF_SZ (2 << 6)

static const char *TAG = "network";
/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
static queue_t * log_server_queue;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define MAX_TRANS_TABLE_SZ (30)


// TODO: set up in func
trans_table_entry_t translation_table[MAX_TRANS_TABLE_SZ];
uint32_t translation_table_sz = 0;

static int s_retry_num = 0;


void translation_table_init(channel_t *channels, uint8_t **ip_addr, uint32_t n_entries) {
    translation_table_sz += n_entries;
    for (int i = 0; i < n_entries; i++) {
        translation_table[i] = (trans_table_entry_t) {
                .flow=channels[i].service_id,
                .addr=channels[i].address,
                .sock_addr={
                    .sin_addr={
                        .s_addr=inet_addr((char *) ip_addr[i]),
                    },
                    .sin_family = AF_INET,
                    .sin_port = htons(PORT),
                }
            };
    }

}


static kaos_error_t get_translation_entry(struct sockaddr_in *sock_addr, service_id_t flow, address_t addr) {
    for (int i = 0; i < translation_table_sz; i++) {
        if (translation_table[i].addr != addr) continue;

        *sock_addr = translation_table[i].sock_addr;
        return KaosSuccess;
    }

    KAOS_LOGW(TAG, "Translation for address %"PRIu8":%"PRIu32" not found", flow, addr);
    return KaosContainerLocationNotFoundError;
}


static esp_err_t KAOS_set_dns_server(esp_netif_t *netif, uint32_t addr, esp_netif_dns_type_t type)
{
    if (addr && (addr != IPADDR_NONE)) {
        esp_netif_dns_info_t dns;
        dns.ip.u_addr.ip4.addr = addr;
        dns.ip.type = IPADDR_TYPE_V4;
        ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, type, &dns));
    }
    return ESP_OK;
}


static void config_set_static_ip(esp_netif_t *netif)
{
    if (esp_netif_dhcpc_stop(netif) != ESP_OK) {
        KAOS_LOGE(TAG, "Failed to stop dhcp client");
        return;
    }
    esp_netif_ip_info_t ip;
    memset(&ip, 0 , sizeof(esp_netif_ip_info_t));
    ip.ip.addr = ipaddr_addr(KAOS_STATIC_IP_SELF);
    ip.netmask.addr = ipaddr_addr(KAOS_STATIC_NETMASK_ADDR);
    ip.gw.addr = ipaddr_addr(KAOS_STATIC_GW_ADDR);
    if (esp_netif_set_ip_info(netif, &ip) != ESP_OK) {
        KAOS_LOGE(TAG, "Failed to set ip info");
        return;
    }
    KAOS_LOGD(TAG, "Success to set static ip: %s, netmask: %s, gw: %s", KAOS_STATIC_IP_SELF, KAOS_STATIC_NETMASK_ADDR, KAOS_STATIC_GW_ADDR);
    ESP_ERROR_CHECK(KAOS_set_dns_server(netif, ipaddr_addr(KAOS_MAIN_DNS_SERVER), ESP_NETIF_DNS_MAIN));
    ESP_ERROR_CHECK(KAOS_set_dns_server(netif, ipaddr_addr(KAOS_BACKUP_DNS_SERVER), ESP_NETIF_DNS_BACKUP));
}


static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        config_set_static_ip(arg);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 100) {
            esp_wifi_connect();
            s_retry_num++;
            KAOS_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        KAOS_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        KAOS_LOGI(TAG, "static ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        sta_netif,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        sta_netif,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = KAOS_WIFI_SSID,
            .password = KAOS_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    KAOS_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        KAOS_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 KAOS_WIFI_SSID, KAOS_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        KAOS_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 KAOS_WIFI_SSID, KAOS_WIFI_PASS);
    } else {
        KAOS_LOGE(TAG, "UNEXPECTED EVENT");
    }
#ifdef CONFIG_KAOS_STATIC_DNS_RESOLVE_TEST
    struct addrinfo *address_info;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int res = getaddrinfo(KAOS_RESOLVE_DOMAIN, NULL, &hints, &address_info);
    if (res != 0 || address_info == NULL) {
        KAOS_LOGE(TAG, "couldn't get hostname for :%s: "
                      "getaddrinfo() returns %d, addrinfo=%p", KAOS_RESOLVE_DOMAIN, res, address_info);
    } else {
        if (address_info->ai_family == AF_INET) {
            struct sockaddr_in *p = (struct sockaddr_in *)address_info->ai_addr;
            KAOS_LOGI(TAG, "Resolved IPv4 address: %s", ipaddr_ntoa((const ip_addr_t*)&p->sin_addr.s_addr));
        }
#if CONFIG_LWIP_IPV6
        else if (address_info->ai_family == AF_INET6) {
            struct sockaddr_in6 *p = (struct sockaddr_in6 *)address_info->ai_addr;
            KAOS_LOGI(TAG, "Resolved IPv6 address: %s", ip6addr_ntoa((const ip6_addr_t*)&p->sin6_addr));
        }
#endif
    }
#endif
    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

#define KEEPALIVE_IDLE              CONFIG_KAOS_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL          CONFIG_KAOS_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT             CONFIG_KAOS_KEEPALIVE_COUNT


static kaos_error_t create_service_queue() {
    log_server_queue = create_queue(KAOS_SIG_QUEUE_SIZE, sizeof(kaos_signal_msg_t));
    if (!log_server_queue) return KaosQueueNotEstablished;

    return KaosSuccess;

}

kaos_error_t service_put(service_msg_t *msg) {
    uint8_t *buffer = calloc(1, msg->buffer_sz);
    if (!buffer) {
        KAOS_LOGE(__FUNCTION__, "Memory allocation error");
        return KaosMemoryAllocationError;
    }

    memcpy(buffer, msg->buffer, msg->buffer_sz);
    msg->buffer = buffer;
        // KAOS_LOGI(__FUNCTION__, "Send to %d", message.dest_address);
    if (!send_to_queue(log_server_queue, msg, 0)) {
        // TODO: Add module name if implemented
        KAOS_LOGW(TAG, "%s: Service queue full.", __FUNCTION__);
        return KaosQueueFullError;
    }

    return KaosSuccess;
}


static kaos_error_t service_get(service_msg_t *message_dest) {
    if (!receive_from_queue(log_server_queue, message_dest, 0)) {
        KAOS_LOGI(TAG, "%s: Service queue empty", __FUNCTION__);
        return KaosNothingToReceive;
    }

    return KaosSuccess;
}


static kaos_error_t receive_from_server(const int sock) {
    int len = 0;
    uint32_t total_len = 0;
    // TODO: rx buffer limit
    uint32_t rx_buff_sz = INIT_RX_BUF_SZ;
    uint8_t _rx_buffer[INIT_RX_BUF_SZ];
    uint8_t *receive_buffer = calloc(1, rx_buff_sz);
    if (!receive_buffer) return KaosMemoryAllocationError;

    // do {
    //     if (total_len >= rx_buff_sz) {
    //         rx_buff_sz *= 2;
    //         KAOS_LOGI(__FUNCTION__, "Realloc buffer to %"PRIu32" bytes", rx_buff_sz);
    //         receive_buffer = realloc(receive_buffer, rx_buff_sz);

    //         if (!receive_buffer) {
    //             KAOS_LOGE(__FUNCTION__, "Can't allocate %"PRIu32" bytes for buffer", rx_buff_sz - total_len);
    //             return KaosMemoryAllocationError;
    //         }
    //         memset(receive_buffer + total_len, 0, rx_buff_sz - total_len);
    //         // KAOS_LOGI(__FUNCTION__, "Set mem to zero");
    //     }

    //     memcpy(receive_buffer + (total_len - len), _rx_buffer, len);
    //     len = recv(sock, _rx_buffer, INIT_RX_BUF_SZ, 0);

    //     if (len < 0) {
    //         KAOS_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
    //         free(receive_buffer);
    //         return KaosTCPRecvError;
    //     } else if (len == 0) {
    //         KAOS_LOGW(TAG, "Stream done");
    //     } else {
    //         total_len += (uint32_t) len;
    //         KAOS_LOGI(TAG, "Received %d bytes, total %"PRIu32"", len, total_len);
    //     }
    // } while (len > 0);

    // for (;;) {
    //     len = recv(sock, _rx_buffer, sizeof(_rx_buffer), 0);
    //     if (len < 0) {
    //         KAOS_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
    //         free(receive_buffer);
    //         return KaosTCPRecvError;
    //     } else if (len == 0) {
    //         /* stream done */
    //         break;
    //     }

    //     /* check that total_len + len won't overflow and won't exceed allowed max */
    //     if (total_len + (uint32_t)len < total_len) {
    //         /* overflow */
    //         free(receive_buffer);
    //         return KaosMemoryAllocationError;
    //     }
    //     if (total_len + (uint32_t)len > KAOS_SOURCE_BUFFER_MAX_SZ) {
    //         KAOS_LOGE(TAG, "Incoming message too large: %"PRIu32, total_len + (uint32_t)len);
    //         free(receive_buffer);
    //         return KaosMemoryAllocationError;
    //     }

    //     /* Ensure capacity */
    //     while (total_len + (uint32_t)len > rx_buff_sz) {
    //         uint32_t new_sz = rx_buff_sz * 2;
    //         if (new_sz <= rx_buff_sz) {
    //             /* overflow */
    //             free(receive_buffer);
    //             return KaosMemoryAllocationError;
    //         }
    //         uint8_t *tmp = realloc(receive_buffer, new_sz);
    //         if (!tmp) {
    //             KAOS_LOGE(__FUNCTION__, "Can't allocate %"PRIu32" bytes for buffer", new_sz);
    //             free(receive_buffer); /* keep expected behavior: free old buffer */
    //             return KaosMemoryAllocationError;
    //         }
    //         receive_buffer = tmp;
    //         /* zero the new region (optional) */
    //         memset(receive_buffer + rx_buff_sz, 0, new_sz - rx_buff_sz);
    //         rx_buff_sz = new_sz;
    //     }

    //     /* append just-read chunk */
    //     memcpy(receive_buffer + total_len, _rx_buffer, (size_t)len);
    //     total_len += (uint32_t)len;
    //     KAOS_LOGI(__FUNCTION__, "Received %d bytes, total %"PRIu32"", len, total_len);
    // }

    

    /* sane maximum to protect against runaway allocations */

    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(TAG, "Heap corruption detected before network receive");
        free(receive_buffer);
        return KaosMemoryAllocationError;
    }

    for (;;) {
        len = recv(sock, _rx_buffer, sizeof(_rx_buffer), 0);
        if (len < 0) {
            KAOS_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
            free(receive_buffer);
            return KaosTCPRecvError;
        }
        if (len == 0) {
            /* stream done */
            break;
        }

        /* bounds checks */
        if ((uint32_t)len > sizeof(_rx_buffer)) {
            free(receive_buffer);
            return KaosMemoryAllocationError;
        }
        if (total_len + (uint32_t)len < total_len) { /* overflow */
            free(receive_buffer);
            return KaosMemoryAllocationError;
        }
        if (total_len + (uint32_t)len > (uint32_t) KAOS_SOURCE_BUFFER_MAX_SZ) {
            KAOS_LOGE(TAG, "Incoming message too large: %"PRIu32, total_len + (uint32_t)len);
            free(receive_buffer);
            return KaosMemoryAllocationError;
        }

        /* ensure capacity, using temporary pointer for realloc */
        while (total_len + (uint32_t)len > rx_buff_sz) {
            uint32_t new_sz = rx_buff_sz * 2;
            if (new_sz <= rx_buff_sz) { /* overflow */
                free(receive_buffer);
                return KaosMemoryAllocationError;
            }
            uint8_t *tmp = realloc(receive_buffer, new_sz);
            if (!tmp) {
                KAOS_LOGE(__FUNCTION__, "Can't allocate %"PRIu32" bytes for buffer", new_sz - total_len);
                free(receive_buffer);
                return KaosMemoryAllocationError;
            }
            /* zero the new region */
            memset(tmp + rx_buff_sz, 0, new_sz - rx_buff_sz);
            receive_buffer = tmp;
            rx_buff_sz = new_sz;
            KAOS_LOGI(__FUNCTION__, "Realloc buffer to %"PRIu32" bytes", rx_buff_sz);
        }

        /* append the just-received chunk */
        memcpy(receive_buffer + total_len, _rx_buffer, (size_t)len);
        total_len += (uint32_t)len;
        KAOS_LOGI(__FUNCTION__, "Received %d bytes, total %"PRIu32"", len, total_len);
    }

    /* Verify heap integrity before handing received buffer to monitor */
    if (!heap_caps_check_integrity_all(true)) {
        KAOS_LOGE(TAG, "Heap corruption detected just after network receive");
        free(receive_buffer);
        return KaosMemoryAllocationError;
    }

    submit_event(EVENT_MSG_RECEIVED, receive_buffer, (uint32_t) total_len);
    // Note: receive_buffer is now owned by the event system and will be freed in monitor_loop
    return KaosSuccess;
}

static int send_to_server(const int sock, service_msg_t msg) {
        // send() can return less bytes than supplied length.
        // Walk-around for robust implementation.
    int total_written = 0;
    int to_write;

    while (((to_write = msg.buffer_sz - total_written) > 0)) {
        int written = send(sock, msg.buffer + total_written, to_write, 0);
        if (written < 0) {
            KAOS_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            // Failed to retransmit, giving up
            return KaosTCPSendError;
        }
        total_written += written;
    }

    return 0;
}


static void tcp_client_task(void *pvParameters) {
    char host_ip[] = KAOS_SERVICE_IP_ADDR;
    int addr_family = 0;
    int ip_protocol = 0;
    service_msg_t service_msg;

    while (1) {
// #if defined(CONFIG_EXAMPLE_IPV4)
        struct sockaddr_in dest_addr;
        inet_pton(AF_INET, host_ip, &dest_addr.sin_addr);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(SERVICE_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
// #elif defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
//         struct sockaddr_storage dest_addr = { 0 };
//         ESP_ERROR_CHECK(get_addr_from_stdin(PORT, SOCK_STREAM, &ip_protocol, &addr_family, &dest_addr));
// #endif
        int sock;
        
        while (1) { 
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            sock =  socket(addr_family, SOCK_STREAM, ip_protocol);
            if (sock < 0) {
                KAOS_LOGE(__FUNCTION__, "Unable to create socket: errno %d", errno);
                break;
            }
            KAOS_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, PORT);

            int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (err != 0) {
                KAOS_LOGE(__FUNCTION__, "TCP Socket unable to connect: errno %d", errno);
                break;
            }
            KAOS_LOGI(__FUNCTION__, "Successfully connected");
                
            
            while (1) {
                // Block on service queue with a timeout to allow other tasks to run
                if (receive_from_queue(log_server_queue, &service_msg, 50) == pdTRUE) {
                    KAOS_LOGI(__FUNCTION__, "Send message to orchestrator");
                    int err = send(sock, service_msg.buffer, service_msg.buffer_sz, 0);
                    if (err < 0) {
                        KAOS_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                        break;
                    }
                    free(service_msg.buffer); // Free the buffer after sending
                    KAOS_LOGI(__FUNCTION__, "Message sent");
                }
            }
        }

        if (sock != -1) {
            KAOS_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }
}


void cleanup(int sock) {
    shutdown(sock, 0);
    close(sock);
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;

#ifdef CONFIG_KAOS_IPV4
    if (addr_family == AF_INET) {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(SERVICE_PORT);
        ip_protocol = IPPROTO_IP;
    }
#endif
#ifdef CONFIG_KAOS_IPV6
    if (addr_family == AF_INET6) {
        struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
        bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
        dest_addr_ip6->sin6_family = AF_INET6;
        dest_addr_ip6->sin6_port = htons(SERVICE_PORT);
        ip_protocol = IPPROTO_IPV6;
    }
#endif

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        KAOS_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    #if defined(CONFIG_KAOS_IPV4) && defined(CONFIG_KAOS_IPV6)
    // Note that by default IPV6 binds to both protocols, it is must be disabled
    // if both protocols used at the same time (used in CI)
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

    KAOS_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        KAOS_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        KAOS_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    KAOS_LOGI(TAG, "Socket bound, port %d", SERVICE_PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        KAOS_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    int sock;
    while (1) {
        KAOS_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            KAOS_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        // Convert ip address to string
#ifdef CONFIG_KAOS_IPV4
        if (source_addr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        }
#endif
#ifdef CONFIG_KAOS_IPV6
        if (source_addr.ss_family == PF_INET6) {
            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
#endif
        KAOS_LOGI(TAG, "Socket accepted ip address: %s", addr_str);
        receive_from_server(sock);
    }

    if (sock) cleanup(sock);

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}


static kaos_error_t send_payload(int sock, uint32_t *payload_n) {
    kaos_error_t err;

    for (;;) {
        // TODO: Reinstate payload notification from previous error 
        message_t msg;
        service_id_t service_id;


        for (int i = *payload_n; i > 0; i--) {
            KAOS_LOGI(TAG, "Get payload");
            if ((err = get_next_container_payload(&msg, &service_id)) > KaosNothingToSend) return err;

            struct sockaddr_in dest_addr;
            // KAOS_LOGI(__FUNCTION__, "Get translation entry");
            if (get_translation_entry(&dest_addr, service_id, msg.dest_address)) {
                free(msg.content);
                KAOS_LOGE(__FUNCTION__, "Container %"PRIu8":%"PRIu32" location not found, message dropped", service_id, msg.dest_address);
                continue;
            }
            // KAOS_LOGI(TAG, "Translation entry of container entry %"PRIu8":%"PRIu32": %s", service_id, msg.dest_address, inet_ntop( AF_INET, &(dest_addr.sin_addr), ip_str, INET_ADDRSTRLEN ));

            char ip_str[INET_ADDRSTRLEN];
            inet_ntoa_r(dest_addr.sin_addr, ip_str, sizeof(ip_str));
            KAOS_LOGI(TAG, "Client sending to %s:%d", ip_str, PORT);
            int err = sendto(sock, msg.content, msg.content_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            free(msg.content);

            if (err < 0) {
                KAOS_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                // TODO: Upon sending error, message is lost - should it be dropped? 
                return KaosUDPSendError;
            }


            KAOS_LOGI(TAG, "Message sent to %s", ip_str);
        }
        
        *payload_n = ulTaskNotifyTake(1, portMAX_DELAY);
    }
}


static void udp_client_task(void *pvParameters) {
    int addr_family = 0;
    int ip_protocol = 0;
    uint32_t payload_n = 0;

    for (;;) {
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);

        if (sock < 0) {
            KAOS_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
    
        kaos_error_t err = send_payload(sock, &payload_n);
        // TODO: Handle kaos UDP error (try to resend?, currently dropped)
        KAOS_LOGE(TAG, "KAOS UDP error %d", err);
        
        if (sock != -1) {
            KAOS_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}


static void udp_server_task(void *pvParameters) {
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    struct sockaddr_in6 dest_addr;

    while (1) {

        if (addr_family == AF_INET) {
            struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
            dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
            dest_addr_ip4->sin_family = AF_INET;
            dest_addr_ip4->sin_port = htons(PORT);
            ip_protocol = IPPROTO_IP;
        } else if (addr_family == AF_INET6) {
            bzero(&dest_addr.sin6_addr.un, sizeof(dest_addr.sin6_addr.un));
            dest_addr.sin6_family = AF_INET6;
            dest_addr.sin6_port = htons(PORT);
            ip_protocol = IPPROTO_IPV6;
        }

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            KAOS_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        KAOS_LOGI(TAG, "Socket created");

#if defined(CONFIG_KAOS_IPV4) && defined(CONFIG_KAOS_IPV6)
        if (addr_family == AF_INET6) {
            // Note that by default IPV6 binds to both protocols, it is must be disabled
            // if both protocols used at the same time (used in CI)
            int opt = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
        }
#endif

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            KAOS_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        KAOS_LOGI(TAG, "Socket bound, port %d", PORT);

        for (;;) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            char rx_buffer[128] = {0};

            KAOS_LOGI(TAG, "Server waiting for data");
            struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
            // KAOS_LOGE("LATENCY_MEASUREMENT_START us", "%lli", get_time_us());
            // Error occurred during receiving
            if (len < 0) {
                KAOS_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }
            // Data received

            receive_container_payload((uint8_t *) rx_buffer);

            // Get the sender's ip address as string
            if (source_addr.ss_family == PF_INET) {
                inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
            } else if (source_addr.ss_family == PF_INET6) {
                inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
            }

            rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string...
            KAOS_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
            // ESP_LOG_BUFFER_HEX(TAG, rx_buffer, len);

            // int err = sendto(sock, rx_buffer, len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
            // if (err < 0) {
            //     KAOS_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            //     break;
            // }
        }

        if (sock != -1) {
            KAOS_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

kaos_error_t send_to_log_server(service_msg_t *msg) {
    return service_put(msg);
}

kaos_error_t get_log_server_msg(service_msg_t *dest) {
    return service_get(dest);
}

task_handle_t *udp_client_handle = NULL;

kaos_error_t notify_and_signal(module_registry_t *sig_registry, queue_t *queue) {
    kaos_error_t err;
    notify_task_give(udp_client_handle);
    
    uint32_t msgs_available;
    messages_available(queue, &msgs_available);
    if (sig_registry && (msgs_available == 1)) {
        kaos_signal_msg_t signal_msg = {
            .signal_type = KAOS_DATA_AVAILABLE,
            .signal_id = 0,
        };

        sig_to_container(sig_registry, &signal_msg);
    }

    return KaosSuccess;
}

// TODO: establish receive queue
// int get_from_log_server()

kaos_error_t start_server(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    KAOS_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ESP_ERROR_CHECK(KAOS_connect());

    kaos_error_t err = create_service_queue();
    if (err) {
        return err;
    }

#ifdef CONFIG_KAOS_IPV4
    create_task(udp_server_task, "udp_server", 4096, (void*)AF_INET, 5, NULL);
#endif
#ifdef CONFIG_KAOS_IPV6
    create_task(udp_server_task, "udp_server", 4096, (void*)AF_INET6, 5, NULL);
#endif
    create_task(udp_client_task, "udp_client", 4096, NULL, 5, &udp_client_handle);
    create_task(tcp_server_task, "tcp_server", 4096, (void*)AF_INET, 5, NULL);
    create_task(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);

    return KaosSuccess;
}

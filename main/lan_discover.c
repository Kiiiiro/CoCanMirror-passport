#include "lan_discover.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "lan_discover";

static bool port80_open(uint32_t ip_be, int timeout_ms) {
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;

    const int flags = fcntl(s, F_GETFL, 0);
    (void)fcntl(s, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = ip_be;
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        // 非阻塞 connect 返回 EINPROGRESS 属正常。
    }

    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    struct timeval tv = { .tv_sec = 0, .tv_usec = timeout_ms * 1000 };
    const int rc = select(s + 1, NULL, &wf, NULL, &tv);
    int err = 0;
    socklen_t elen = sizeof(err);
    if (rc > 0) (void)getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
    close(s);
    return rc > 0 && err == 0;
}

static bool metrics_matches(uint32_t ip_be) {
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;

    struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = ip_be;

    bool found = false;
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char req[160];
        snprintf(req, sizeof(req),
                 "GET /metrics HTTP/1.0\r\nHost: %u.%u.%u.%u\r\nConnection: close\r\n\r\n",
                 (unsigned)(ntohl(ip_be) >> 24) & 0xFF, (unsigned)(ntohl(ip_be) >> 16) & 0xFF,
                 (unsigned)(ntohl(ip_be) >> 8) & 0xFF, (unsigned)ntohl(ip_be) & 0xFF);
        if (send(s, req, strlen(req), 0) > 0) {
            char buf[512];
            int total = 0;
            for (;;) {
                const int n = recv(s, buf + total, sizeof(buf) - 1 - (size_t)total, 0);
                if (n <= 0) break;
                total += n;
                buf[total] = '\0';
                if (strstr(buf, "ionbridge_")) {
                    found = true;
                    break;
                }
                if (total >= (int)sizeof(buf) - 1) break;
            }
        }
    }
    close(s);
    return found;
}

bool lan_discover_find(char *ip_out, size_t ip_out_len, uint32_t budget_ms) {
    if (!ip_out || ip_out_len < 8) return false;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;

    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) return false;

    const uint32_t self = ntohl(info.ip.addr);
    const uint32_t mask = ntohl(info.netmask.addr);
    const uint32_t base = self & mask;
    const uint32_t host_bits = ~mask;
    uint32_t count = host_bits > 1024 ? 254 : (host_bits > 1 ? host_bits - 1 : 0);
    if (count == 0) return false;

    ESP_LOGI(TAG, "扫描网段 %u.%u.%u.0/%u",
             (unsigned)(base >> 24) & 0xFF, (unsigned)(base >> 16) & 0xFF,
             (unsigned)(base >> 8) & 0xFF, (unsigned)__builtin_popcount(mask));

    const uint32_t start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    const uint32_t batch = 6;

    for (uint32_t h = 1; h <= count; h += batch) {
        const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - start_ms > budget_ms) {
            ESP_LOGW(TAG, "扫描超时");
            return false;
        }

        uint32_t tried[8];
        int n_tried = 0;
        for (uint32_t k = h; k < h + batch && k <= count && n_tried < (int)batch; k++) {
            const uint32_t host = base + k;
            if (host == self) continue;
            if (port80_open(htonl(host), 300)) tried[n_tried++] = host;
        }

        for (int i = 0; i < n_tried; i++) {
            if (metrics_matches(htonl(tried[i]))) {
                snprintf(ip_out, ip_out_len, "%u.%u.%u.%u",
                         (unsigned)(tried[i] >> 24) & 0xFF, (unsigned)(tried[i] >> 16) & 0xFF,
                         (unsigned)(tried[i] >> 8) & 0xFF, (unsigned)tried[i] & 0xFF);
                ESP_LOGI(TAG, "找到小电拼: %s", ip_out);
                return true;
            }
        }
    }
    return false;
}

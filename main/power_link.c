#include "power_link.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "sys/time.h"
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "sys/socket.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "power_link";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define HTTP_BODY_MAX 8192
#define HTTP_FAIL_THRESHOLD 3
// 判定“该口确实在输出”的最小电流/电压。空载/已关闭的口都是 0V/0mA，
// 因此只有出现明显输出（哪怕是小电流涓充）才把开关状态纠正为 ON。
#define POWER_ON_CURRENT_MA 10
#define POWER_ON_VOLTAGE_MV 4000
#define POLL_INTERVAL_MS 1000
#define CLOUD_POLL_MS 3000
#define MCP_SESSION_MAX 64

static SemaphoreHandle_t s_lock;
static power_link_view_t s_view;
static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static app_config_t s_cfg;
static TaskHandle_t s_task;
static volatile bool s_stop;
static bool s_wifi_ready;
static bool s_wifi_started;
static int s_consecutive_fail;

// 云端 MCP 会话：initialize 之后由服务端下发的 Mcp-Session-Id。
static char s_session[MCP_SESSION_MAX];
static bool s_mcp_ready;
static uint32_t s_rpc_id = 1;
static int s_last_status;

// AUTO 模式的双通道状态：优先局域网，连续失败后切云端，并定期回探局域网。

// 端口输出状态的持久化掩码(bit0 = 端口1)，重启后按它恢复界面。
static uint16_t s_port_mask = 0x003F;

// 息屏省电状态：降频 + Wi-Fi MAX_MODEM。
static volatile bool s_idle;
static bool s_ps_applied;

// 按需拉取 PD/线材信息的端口(1-based)，0 表示无请求。
static volatile uint8_t s_pd_request;
static uint16_t s_poll_ms = 1000;
static volatile bool s_scanning;
static volatile power_discover_t s_discover = POWER_DISCOVER_IDLE;
static bool s_time_synced;
static bool s_idle_pause = true;

// 端口开关请求队列：由同一个任务串行处理，复用一条 MCP 会话。
typedef struct {
    uint8_t port;  // 1-based
    bool on;
} port_cmd_t;
static QueueHandle_t s_cmd_queue;

// 复用缓冲区：避免在任务栈上放 8KB。
static char s_body[HTTP_BODY_MAX];

static void view_set_state(power_link_state_t state, int http_status) {
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdPASS) return;
    s_view.state = state;
    s_view.http_status = http_status;
    xSemaphoreGive(s_lock);
}

static void view_record_success(const power_snapshot_t *snap, const bool *port_on, const bool *port_on_valid,
                                bool from_cloud) {
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdPASS) return;

    s_view.metrics = *snap;
    s_view.state = POWER_LINK_LIVE;
    s_view.via_cloud = true;
    s_view.http_status = 200;
    s_view.poll_ok++;
    s_view.last_ok_ms = esp_timer_get_time() / 1000;
    s_view.seq++;

    // 只合并“确实还在输出”的端口（检测到电流）。关闭状态无法从设备查询，
    // 一律以用户命令的结果为准，避免被读数把 OFF 又判回 ON。
    if (port_on && port_on_valid) {
        for (int i = 0; i < POWER_METRICS_MAX_PORTS; i++) {
            if (!port_on_valid[i]) continue;
            s_view.port_on[i] = port_on[i];
            s_view.port_on_valid[i] = true;
        }
    }
    (void)from_cloud;

    uint32_t total_mw = 0;
    const uint8_t head = s_view.history_head;
    for (int i = 0; i < POWER_METRICS_MAX_PORTS; i++) {
        const uint32_t mw = power_port_milliwatts(&snap->ports[i]);
        s_view.port_history[i][head] = mw;
        total_mw += mw;
    }
    s_view.total_history[head] = total_mw;
    s_view.history_head = (uint8_t)((head + 1) % POWER_HISTORY_LEN);
    if (s_view.history_len < POWER_HISTORY_LEN) s_view.history_len++;

    xSemaphoreGive(s_lock);
}

static void view_record_failure(int http_status, const char *reason) {
    s_consecutive_fail++;
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdPASS) return;
    s_view.poll_fail++;
    s_view.http_status = http_status;
    if (s_consecutive_fail >= HTTP_FAIL_THRESHOLD) s_view.state = POWER_LINK_HTTP_ERROR;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "轮询失败(%s): status=%d 连续=%d", reason, http_status, s_consecutive_fail);
}

// 检测到某口确实有输出时，把持久化掩码也纠正为 ON（例如官方 App 重新打开了）。
static void port_mask_note_on(const bool *on, const bool *valid) {
    if (!on || !valid) return;
    bool changed = false;
    for (int i = 0; i < POWER_METRICS_MAX_PORTS; i++) {
        if (!valid[i] || !on[i]) continue;
        const uint16_t bit = (uint16_t)(1u << i);
        if (!(s_port_mask & bit)) {
            s_port_mask |= bit;
            changed = true;
        }
    }
    if (changed) (void)app_config_set_port_mask(s_port_mask);
}

static void view_update_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdPASS) return;
    s_view.rssi = (int8_t)ap.rssi;
    s_view.rssi_valid = true;
    xSemaphoreGive(s_lock);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
        ESP_LOGW(TAG, "Wi-Fi 断开 reason=%d，准备重连", disc ? disc->reason : -1);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        char ip[16] = {0};
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdPASS) {
            strncpy(s_view.ip, ip, sizeof(s_view.ip) - 1);
            s_view.ip[sizeof(s_view.ip) - 1] = '\0';
            xSemaphoreGive(s_lock);
        }
        xEventGroupClearBits(s_events, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "获取 IP: %s", ip);
    }
}

static esp_err_t wifi_init_once(void) {
    if (s_wifi_ready) return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!s_netif) s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    s_wifi_ready = true;
    return ESP_OK;
}

static void copy_fixed(uint8_t *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

// ---------------------------------------------------------------- 云端 MCP

// 响应头里的 Mcp-Session-Id 只能通过事件回调拿到（get_header 读的是请求头）。
typedef struct {
    char session[MCP_SESSION_MAX];
} mcp_header_ctx_t;

static esp_err_t mcp_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        evt->user_data) {
        if (strcasecmp(evt->header_key, "mcp-session-id") == 0) {
            mcp_header_ctx_t *ctx = (mcp_header_ctx_t *)evt->user_data;
            copy_fixed((uint8_t *)ctx->session, sizeof(ctx->session), evt->header_value);
        }
    }
    return ESP_OK;
}

// 一次 JSON-RPC over Streamable HTTP。响应体写入 s_body，会话 ID 写入 s_session。
static esp_http_client_handle_t s_mcp_client;

static void mcp_client_drop(void) {
    if (s_mcp_client) {
        esp_http_client_close(s_mcp_client);
        esp_http_client_cleanup(s_mcp_client);
        s_mcp_client = NULL;
    }
}

static esp_http_client_handle_t mcp_client_get(mcp_header_ctx_t *ctx) {
    if (s_mcp_client) {
        (void)esp_http_client_set_user_data(s_mcp_client, ctx);
        return s_mcp_client;
    }

    esp_http_client_config_t cfg = {
        .url = s_cfg.mcp_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 9000,
        .buffer_size = 1024,
        .skip_cert_common_name_check = true,
        .event_handler = mcp_http_event,
        .user_data = ctx,
        .keep_alive_enable = true,
    };
    s_mcp_client = esp_http_client_init(&cfg);
    if (!s_mcp_client) return NULL;

    esp_http_client_set_header(s_mcp_client, "Content-Type", "application/json");
    esp_http_client_set_header(s_mcp_client, "Accept", "application/json, text/event-stream");
    esp_http_client_set_header(s_mcp_client, "Connection", "keep-alive");
    return s_mcp_client;
}

// 同一条连接复用：open/close 不调用，只有出错时才丢弃重连。
static int mcp_post(const char *request, size_t len) {
    int status = -1;
    s_body[0] = '\0';

    for (int attempt = 0; attempt < 2; attempt++) {
        mcp_header_ctx_t header_ctx = {0};
        esp_http_client_handle_t client = mcp_client_get(&header_ctx);
        if (!client) break;

        if (s_session[0]) esp_http_client_set_header(client, "Mcp-Session-Id", s_session);
        else esp_http_client_set_header(client, "Mcp-Session-Id", NULL);

        if (esp_http_client_open(client, (int)len) != ESP_OK ||
            esp_http_client_write(client, request, (int)len) != (int)len) {
            mcp_client_drop();
            continue;
        }

        (void)esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (header_ctx.session[0]) {
            copy_fixed((uint8_t *)s_session, sizeof(s_session), header_ctx.session);
        }

        size_t used = 0;
        while (used < sizeof(s_body) - 1) {
            const int read = esp_http_client_read(client, s_body + used, sizeof(s_body) - 1 - used);
            if (read <= 0) break;
            used += (size_t)read;
        }
        s_body[used] = '\0';

        if (used >= sizeof(s_body) - 1) {
            // 响应超出缓冲，连接里的剩余数据无法丢弃，只能换新连接。
            mcp_client_drop();
        }
        mcp_client_drop();
        if (status > 0) return status;
    }

    if (status <= 0) {
        char host[64];
        app_config_url_host(s_cfg.mcp_url, host, sizeof(host));
        ESP_LOGW(TAG, "MCP 连接失败 host=%s", host);
    }
    return status;
}

static bool mcp_initialize(void) {
    char request[256];
    const int n = snprintf(request, sizeof(request),
                           "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"initialize\",\"params\":{"
                           "\"protocolVersion\":\"2025-03-26\",\"capabilities\":{},"
                           "\"clientInfo\":{\"name\":\"ai-passport\",\"version\":\"1.0.0\"}}}",
                           (unsigned)s_rpc_id++);
    if (n <= 0 || (size_t)n >= sizeof(request)) return false;

    const int status = mcp_post(request, (size_t)n);
    s_last_status = status;
    if (status != 200 || s_session[0] == '\0') {
        ESP_LOGW(TAG, "MCP initialize 失败 HTTP %d", status);
        s_mcp_ready = false;
        return false;
    }

    const char notification[] = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    (void)mcp_post(notification, sizeof(notification) - 1);

    s_mcp_ready = true;
    ESP_LOGI(TAG, "MCP 会话已建立");
    return true;
}

// 解析 tools/call 结果：result.content[0].text 里是一段 JSON 字符串。
// 开关状态无法直接查询：只有“确实有电流”才能证明该口在输出，据此把状态纠正为 ON；
// 其余情况保持上次命令的结果（关闭的口 / 空载的口都可能是 0mA）。
static bool mcp_parse_ports(power_snapshot_t *snap, bool *port_on, bool *port_on_valid) {
    cJSON *root = cJSON_Parse(s_body);
    if (!root) return false;

    bool ok = false;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *content = result ? cJSON_GetObjectItemCaseSensitive(result, "content") : NULL;
    cJSON *first = content ? cJSON_GetArrayItem(content, 0) : NULL;
    cJSON *text = first ? cJSON_GetObjectItemCaseSensitive(first, "text") : NULL;

    if (cJSON_IsString(text) && text->valuestring) {
        cJSON *payload = cJSON_Parse(text->valuestring);
        if (payload) {
            memset(snap, 0, sizeof(*snap));
            memset(port_on, 0, sizeof(bool) * POWER_METRICS_MAX_PORTS);
            memset(port_on_valid, 0, sizeof(bool) * POWER_METRICS_MAX_PORTS);

            cJSON *ports = cJSON_GetObjectItemCaseSensitive(payload, "ports");
            cJSON *port = NULL;
            cJSON_ArrayForEach(port, ports) {
                cJSON *id = cJSON_GetObjectItemCaseSensitive(port, "port");
                if (!cJSON_IsNumber(id)) continue;
                const int index = id->valueint - 1;  // 设备端口从 1 开始
                if (index < 0 || index >= POWER_METRICS_MAX_PORTS) continue;

                cJSON *vout = cJSON_GetObjectItemCaseSensitive(port, "vout_mv");
                cJSON *iout = cJSON_GetObjectItemCaseSensitive(port, "iout_ma");
                cJSON *proto = cJSON_GetObjectItemCaseSensitive(port, "fc_protocol");
                cJSON *temp = cJSON_GetObjectItemCaseSensitive(port, "die_temperature");

                snap->ports[index].has_voltage = true;
                snap->ports[index].has_current = true;
                snap->ports[index].voltage_mv = cJSON_IsNumber(vout) ? (uint32_t)vout->valuedouble : 0;
                snap->ports[index].current_ma = cJSON_IsNumber(iout) ? (uint32_t)iout->valuedouble : 0;
                if (cJSON_IsString(proto) && proto->valuestring) {
                    strncpy(snap->ports[index].protocol, proto->valuestring,
                            sizeof(snap->ports[index].protocol) - 1);
                }
                if (cJSON_IsString(temp) && temp->valuestring) {
                    strncpy(snap->ports[index].temperature, temp->valuestring,
                            sizeof(snap->ports[index].temperature) - 1);
                }
                if (index + 1 > snap->port_count) snap->port_count = index + 1;

                if (snap->ports[index].current_ma > POWER_ON_CURRENT_MA ||
                    snap->ports[index].voltage_mv > POWER_ON_VOLTAGE_MV) {
                    port_on[index] = true;
                    port_on_valid[index] = true;
                }
            }
            cJSON_Delete(payload);
            ok = snap->port_count > 0;
        }
    }

    cJSON_Delete(root);
    return ok;
}

static bool mcp_fetch_ports(power_snapshot_t *snap, bool *port_on, bool *port_on_valid) {
    if (!s_mcp_ready && !mcp_initialize()) {
        s_last_status = -1;
        return false;
    }

    char request[192];
    const int n = snprintf(request, sizeof(request),
                           "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"get_port_details\",\"arguments\":{}}}",
                           (unsigned)s_rpc_id++);
    if (n <= 0 || (size_t)n >= sizeof(request)) return false;

    const int status = mcp_post(request, (size_t)n);
    s_last_status = status;

    if (status != 200) {
        ESP_LOGW(TAG, "MCP tools/call 失败 HTTP %d", status);
        // 会话过期或凭证被吊销：下次重新握手。
        if (status == 400 || status == 401 || status == 404) s_mcp_ready = false;
        return false;
    }
    return mcp_parse_ports(snap, port_on, port_on_valid);
}

// ---------------- PD / 线材信息 ----------------

static void json_str(cJSON *obj, const char *key, char *dst, size_t len) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, len - 1);
        dst[len - 1] = '\0';
    }
}

static bool json_bool(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsTrue(item);
}

static bool parse_pd_for_port(int port, power_pd_info_t *out) {
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(s_body);
    if (!root) return false;

    bool found = false;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *content = result ? cJSON_GetObjectItemCaseSensitive(result, "content") : NULL;
    cJSON *first = content ? cJSON_GetArrayItem(content, 0) : NULL;
    cJSON *text = first ? cJSON_GetObjectItemCaseSensitive(first, "text") : NULL;

    if (cJSON_IsString(text) && text->valuestring) {
        cJSON *payload = cJSON_Parse(text->valuestring);
        if (payload) {
            cJSON *ports = cJSON_GetObjectItemCaseSensitive(payload, "ports");
            cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, ports) {
                cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "port");
                if (!cJSON_IsNumber(id) || id->valueint != port) continue;

                out->valid = true;
                out->port = (uint8_t)port;
                json_str(entry, "cable_max_vbus_voltage", out->cable_max_v, sizeof(out->cable_max_v));
                json_str(entry, "cable_max_vbus_current", out->cable_max_a, sizeof(out->cable_max_a));
                json_str(entry, "cable_usb_highest_speed", out->cable_speed, sizeof(out->cable_speed));
                json_str(entry, "cable_active_element", out->cable_element, sizeof(out->cable_element));
                out->pps = json_bool(entry, "pps_charging_supported");
                out->epr = json_bool(entry, "request_epr_mode_capable");
                out->avs = json_bool(entry, "request_ppsavs");
                out->emarker = json_bool(entry, "has_emarker");

                cJSON *vid = cJSON_GetObjectItemCaseSensitive(entry, "cable_vid");
                cJSON *pid = cJSON_GetObjectItemCaseSensitive(entry, "cable_pid");
                out->cable_vid = cJSON_IsNumber(vid) ? (uint16_t)vid->valueint : 0;
                out->cable_pid = cJSON_IsNumber(pid) ? (uint16_t)pid->valueint : 0;
                found = true;
                break;
            }
            cJSON_Delete(payload);
        }
    }

    cJSON_Delete(root);
    return found;
}

// 拉取指定端口的 PD/线材信息并发布到 view。端口未激活时设备可能不返回该口，
// 此时保留旧值但标记失效，界面显示“无 PD 数据”。
// 单独取一次 get_port_details 拿该口当前的快充协议名：局域网 /metrics 不含此字段，
// 而本地轮询在 AUTO 模式下通常走局域网，所以详情页需要补这一下。
static bool mcp_fetch_protocol(int port, char *out, size_t len) {
    out[0] = '\0';

    char request[192];
    const int n = snprintf(request, sizeof(request),
                           "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"get_port_details\",\"arguments\":{}}}",
                           (unsigned)s_rpc_id++);
    if (n <= 0 || (size_t)n >= sizeof(request)) return false;
    if (mcp_post(request, (size_t)n) != 200) return false;

    cJSON *root = cJSON_Parse(s_body);
    if (!root) return false;

    bool found = false;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *content = result ? cJSON_GetObjectItemCaseSensitive(result, "content") : NULL;
    cJSON *first = content ? cJSON_GetArrayItem(content, 0) : NULL;
    cJSON *text = first ? cJSON_GetObjectItemCaseSensitive(first, "text") : NULL;

    if (cJSON_IsString(text) && text->valuestring) {
        cJSON *payload = cJSON_Parse(text->valuestring);
        if (payload) {
            cJSON *ports = cJSON_GetObjectItemCaseSensitive(payload, "ports");
            cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, ports) {
                cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "port");
                if (!cJSON_IsNumber(id) || id->valueint != port) continue;
                cJSON *proto = cJSON_GetObjectItemCaseSensitive(entry, "fc_protocol");
                if (cJSON_IsString(proto) && proto->valuestring) {
                    strncpy(out, proto->valuestring, len - 1);
                    out[len - 1] = '\0';
                    found = true;
                }
                break;
            }
            cJSON_Delete(payload);
        }
    }

    cJSON_Delete(root);
    return found;
}

static void fetch_pd_status(int port) {
    if (!s_cfg.mcp_url[0]) return;
    if (!s_mcp_ready && !mcp_initialize()) return;

    char request[176];
    const int n = snprintf(request, sizeof(request),
                           "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"get_port_pd_status\",\"arguments\":{}}}",
                           (unsigned)s_rpc_id++);
    if (n <= 0 || (size_t)n >= sizeof(request)) return;

    const int status = mcp_post(request, (size_t)n);
    s_last_status = status;
    if (status != 200) {
        if (status == 400 || status == 401 || status == 404) s_mcp_ready = false;
        return;
    }

    power_pd_info_t info;
    const bool found = parse_pd_for_port(port, &info);
    info.tried = true;
    if (!found) info.valid = false;

    if (found) {
        if (!mcp_fetch_protocol(port, info.protocol, sizeof(info.protocol))) {
            if (info.avs && info.epr) snprintf(info.protocol, sizeof(info.protocol), "PD EPR AVS");
            else if (info.avs) snprintf(info.protocol, sizeof(info.protocol), "PD AVS");
            else if (info.pps) snprintf(info.protocol, sizeof(info.protocol), "PD PPS");
            else if (info.epr) snprintf(info.protocol, sizeof(info.protocol), "PD EPR");
        }
    }

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdPASS) {
        s_view.pd[port - 1] = info;
        xSemaphoreGive(s_lock);
    }
}

static bool cloud_poll(void) {
    power_snapshot_t snap;
    bool port_on[POWER_METRICS_MAX_PORTS];
    bool port_on_valid[POWER_METRICS_MAX_PORTS];

    if (!mcp_fetch_ports(&snap, port_on, port_on_valid)) {
        view_record_failure(s_last_status, "MCP 调用失败");
        return false;
    }
    view_record_success(&snap, port_on, port_on_valid, true);
    port_mask_note_on(port_on, port_on_valid);
    return true;
}

// 把端口(1-based)打开/关闭。返回 true 表示服务端确认已执行。
static bool mcp_set_port(int port, bool on) {
    if (!s_cfg.mcp_url[0]) return false;
    if (!s_mcp_ready && !mcp_initialize()) return false;

    char request[192];
    const int n = snprintf(request, sizeof(request),
                           "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"%s\",\"arguments\":{\"ports\":[%d]}}}",
                           (unsigned)s_rpc_id++, on ? "turn_on_port" : "turn_off_port", port);
    if (n <= 0 || (size_t)n >= sizeof(request)) return false;

    const int status = mcp_post(request, (size_t)n);
    s_last_status = status;
    if (status != 200) {
        ESP_LOGW(TAG, "端口 %d 开关失败 HTTP %d", port, status);
        if (status == 400 || status == 401 || status == 404) s_mcp_ready = false;
        return false;
    }
    // 成功响应是纯文本，例如 "ports [5] turned off"。
    return strstr(s_body, "turned") != NULL;
}


static bool mcp_all_set(bool on) {
    if (!s_cfg.mcp_url[0]) return false;
    if (!s_mcp_ready && !mcp_initialize()) return false;

    char request[192];
    const int n = snprintf(request, sizeof(request),
                           "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"%s\",\"arguments\":{\"ports\":[1,2,3,4,5]}}}",
                           (unsigned)s_rpc_id++, on ? "turn_on_port" : "turn_off_port");
    if (n <= 0 || (size_t)n >= sizeof(request)) return false;

    const int status = mcp_post(request, (size_t)n);
    s_last_status = status;
    if (status != 200) {
        ESP_LOGW(TAG, "切换全部接口失败 HTTP %d", status);
        if (status == 400 || status == 401 || status == 404) s_mcp_ready = false;
        return false;
    }
    return strstr(s_body, "turned") != NULL;
}

static void handle_port_command(const port_cmd_t *cmd) {
    const bool all = (cmd->port == 0);
    const bool ok = all ? mcp_all_set(cmd->on) : mcp_set_port(cmd->port, cmd->on);
    if (ok && all) {
        ESP_LOGI(TAG, "已%s全部接口", cmd->on ? "打开" : "关闭");
        s_port_mask = cmd->on ? 0x003F : 0;
        (void)app_config_set_port_mask(s_port_mask);
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdPASS) {
            for (int i = 0; i < POWER_METRICS_MAX_PORTS; i++) {
                s_view.port_on[i] = cmd->on;
                s_view.port_on_valid[i] = true;
                s_view.pd[i].valid = false;
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(900));
    } else if (ok) {
        ESP_LOGI(TAG, "端口 %u 已%s", (unsigned)cmd->port, cmd->on ? "打开" : "关闭");
        // 先按命令结果更新界面：设备生效有延迟，立刻回读会拿到旧值，
        // 导致界面仍显示 ON，下一次长按又发一次“关闭”。
        const uint16_t bit = (uint16_t)(1u << (cmd->port - 1));
        s_port_mask = cmd->on ? (uint16_t)(s_port_mask | bit) : (uint16_t)(s_port_mask & ~bit);
        (void)app_config_set_port_mask(s_port_mask);

        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdPASS) {
            s_view.port_on[cmd->port - 1] = cmd->on;
            s_view.port_on_valid[cmd->port - 1] = true;
            if (!cmd->on) s_view.pd[cmd->port - 1].valid = false;
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(900));  // 等设备真正切换输出
    } else {
        ESP_LOGW(TAG, "端口 %u %s失败", (unsigned)cmd->port, cmd->on ? "打开" : "关闭");
    }

    // 刷新真实状态（关闭的口 vin 为 0）。刚被操作的口以命令结果为准，
    // 避免抓到来不及生效的旧读数。
    power_snapshot_t snap;
    bool port_on[POWER_METRICS_MAX_PORTS];
    bool port_on_valid[POWER_METRICS_MAX_PORTS];
    if (s_mcp_ready && mcp_fetch_ports(&snap, port_on, port_on_valid)) {
        if (ok) {
            if (all) {
                for (int i = 0; i < POWER_METRICS_MAX_PORTS; i++) {
                    port_on[i] = cmd->on;
                    port_on_valid[i] = true;
                }
            } else {
                port_on[cmd->port - 1] = cmd->on;
                port_on_valid[cmd->port - 1] = true;
            }
        }
        view_record_success(&snap, port_on, port_on_valid, true);
    }

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdPASS) {
        s_view.ctl_state = ok ? POWER_CTL_OK : POWER_CTL_FAIL;
        s_view.ctl_error = s_last_status;
        xSemaphoreGive(s_lock);
    }
}


esp_err_t power_link_control_all(bool on) {
    if (!s_cmd_queue) return ESP_ERR_INVALID_STATE;
    if (!s_cfg.mcp_url[0]) return ESP_ERR_NOT_SUPPORTED;

    const port_cmd_t cmd = { .port = 0, .on = on };
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50)) != pdPASS) return ESP_ERR_TIMEOUT;

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdPASS) {
        s_view.ctl_state = POWER_CTL_BUSY;
        s_view.ctl_port = 0;
        s_view.ctl_on = on;
        s_view.ctl_error = 0;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

esp_err_t power_link_control(int port, bool on) {
    if (port < 1 || port > POWER_METRICS_MAX_PORTS) return ESP_ERR_INVALID_ARG;
    if (!s_cmd_queue) return ESP_ERR_INVALID_STATE;
    if (!s_cfg.mcp_url[0]) return ESP_ERR_NOT_SUPPORTED;

    const port_cmd_t cmd = { .port = (uint8_t)port, .on = on };
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50)) != pdPASS) return ESP_ERR_TIMEOUT;

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdPASS) {
        s_view.ctl_state = POWER_CTL_BUSY;
        s_view.ctl_port = port;
        s_view.ctl_on = on;
        s_view.ctl_error = 0;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

power_discover_t power_link_discover_state(void) {
    return s_discover;
}

void power_link_set_scanning(bool scanning) {
    s_scanning = scanning;
}

void power_link_set_poll_ms(uint16_t ms) {
    if (ms == 500 || ms == 1000 || ms == 3000) s_poll_ms = ms;
}

void power_link_set_idle_pause(bool pause) {
    s_idle_pause = pause;
    if (pause && s_task) xTaskNotifyGive(s_task);
}

void power_link_log_tools(void) {
    if (!s_mcp_ready && !mcp_initialize()) return;
    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/list\",\"params\":{}}";
    const int st = mcp_post(req, strlen(req));
    ESP_LOGI(TAG, "tools/list status=%d body=%.1500s", st, s_body);
}

void power_link_log_details(void) {
    if (!s_mcp_ready && !mcp_initialize()) return;
    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":98,\"method\":\"tools/call\","
                      "\"params\":{\"name\":\"get_port_details\",\"arguments\":{}}}";
    const int st = mcp_post(req, strlen(req));
    ESP_LOGI(TAG, "details status=%d len=%u body=%.1500s", st, (unsigned)strlen(s_body), s_body);
}

void power_link_request_tool_list(void) {
    if (!s_task) return;
    s_pd_request = 0xFE;
    xTaskNotifyGive(s_task);
}

esp_err_t power_link_request_pd(int port) {
    if (port < 1 || port > POWER_METRICS_MAX_PORTS) return ESP_ERR_INVALID_ARG;
    if (!s_cfg.mcp_url[0]) return ESP_ERR_NOT_SUPPORTED;
    if (!s_task) return ESP_ERR_INVALID_STATE;
    s_pd_request = (uint8_t)port;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void power_link_set_idle(bool idle) {
    if (s_idle == idle) return;
    s_idle = idle;
    // 唤醒时立即轮询一次，不让用户盯着旧数据等 10s/60s。
    if (!idle && s_task) xTaskNotifyGive(s_task);
}

static void power_link_task(void *arg) {
    (void)arg;
    for (;;) {
        if (s_stop) break;

        esp_err_t err = wifi_init_once();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(err));
            view_set_state(POWER_LINK_WIFI_FAILED, 0);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (s_scanning) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        view_set_state(POWER_LINK_WIFI_CONNECTING, 0);
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

        wifi_config_t wifi_cfg = {0};
        copy_fixed(wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), s_cfg.ssid);
        copy_fixed(wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), s_cfg.password);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "设置 Wi-Fi 配置失败: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        if (!s_wifi_started) {
            // STA_START 事件里会自动发起第一次连接。
            err = esp_wifi_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
                view_set_state(POWER_LINK_WIFI_FAILED, 0);
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            s_wifi_started = true;
        } else {
            err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "发起连接失败: %s", esp_err_to_name(err));
            }
        }

        const EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                     pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
        if (!(bits & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "连接 %s 超时，稍后重试", s_cfg.ssid);
            view_set_state(POWER_LINK_WIFI_FAILED, 0);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        const bool has_cloud = s_cfg.mcp_url[0] != '\0';
        (void)has_cloud;
        s_consecutive_fail = 0;

        if (!s_time_synced) {
            // 后台异步对时，不阻塞首轮数据；当前 TLS 不校验证书，时间只影响日志。
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) s_time_synced = true;
        }

        bool force_poll = false;
        (void)force_poll;
        while (!s_stop && (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT)) {
            // 息屏时切到 MAX_MODEM 省电；轮询间隔也相应拉长。
            if (s_wifi_started && s_idle != s_ps_applied) {
                (void)esp_wifi_set_ps(s_idle ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
                s_ps_applied = s_idle;
                ESP_LOGI(TAG, "息屏省电: %s", s_idle ? "MAX_MODEM / 降频" : "恢复低延迟");
            }

            const bool idle = s_idle;
            const bool paused = (idle && s_idle_pause) || s_scanning;

            if (!paused) {
                // 先处理详情页的 PD/线材请求，避免排在轮询后面多等一个周期。
                if (s_pd_request == 0xFE) {
                    s_pd_request = 0;
                    if (!s_mcp_ready) (void)mcp_initialize();
                    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":88,\"method\":\"tools/list\",\"params\":{}}";
                    const int st = mcp_post(req, strlen(req));
                    ESP_LOGI(TAG, "tools/list status=%d body=%.2200s", st, s_body);
                } else if (s_pd_request) {
                    const int pd_port = s_pd_request;
                    s_pd_request = 0;
                    fetch_pd_status(pd_port);
                }

                const bool ok = cloud_poll();
                if (ok) s_consecutive_fail = 0;

                // 端口开关请求串行执行，和轮询共用同一条 MCP 会话。
                port_cmd_t cmd;
                if (s_cmd_queue && xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
                    handle_port_command(&cmd);
                }


                view_update_rssi();
            }

            uint32_t wait_ms;
            if (force_poll) {
                force_poll = false;
                wait_ms = 0;  // 刚切换链路，立即拉一次
            } else if (paused) {
                // 暂停轮询，只做 1s 一次的连接巡检；唤醒会立即 notify。
                wait_ms = 1000;
            } else if (idle) {
                wait_ms = 60000;
            } else {
                wait_ms = s_poll_ms;
            }
            // 唤醒时 UI 会 notify 本任务，立即刷新一次而不是等满间隔。
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t power_link_start(const app_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_task) return ESP_ERR_INVALID_STATE;

    s_cfg = *cfg;
    s_idle_pause = app_config_get_idle_pause() != 0;
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    s_cmd_queue = xQueueCreate(4, sizeof(port_cmd_t));
    if (!s_lock || !s_events || !s_cmd_queue) return ESP_ERR_NO_MEM;

    memset(&s_view, 0, sizeof(s_view));
    s_view.state = POWER_LINK_BOOT;
    s_view.http_status = 0;

    // 按上次命令恢复开关状态：设备没有查询接口，重启后只能沿用本地记录。
    s_port_mask = app_config_get_port_mask();
    for (int i = 0; i < POWER_METRICS_MAX_PORTS; i++) {
        s_view.port_on[i] = ((s_port_mask >> i) & 1u) != 0;
        s_view.port_on_valid[i] = true;
    }

    if (xTaskCreate(power_link_task, "power_link", 6144, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (s_cfg.mode == APP_LINK_CLOUD) {
        ESP_LOGI(TAG, "开始连接 %s，云端 MCP 模式", s_cfg.ssid);
    } else if (s_cfg.mode == APP_LINK_AUTO) {
        ESP_LOGI(TAG, "开始连接 %s，自动模式(局域网优先)", s_cfg.ssid);
    } else {
        ESP_LOGI(TAG, "开始连接 %s，充电器地址 %s", s_cfg.ssid, s_cfg.host);
    }
    return ESP_OK;
}

void power_link_get(power_link_view_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdPASS) return;
    *out = s_view;
    xSemaphoreGive(s_lock);
}

const char *power_link_state_text(power_link_state_t state) {
    switch (state) {
    case POWER_LINK_LIVE:
        return "LIVE";
    case POWER_LINK_HTTP_ERROR:
        return "NO DATA";
    case POWER_LINK_WIFI_FAILED:
        return "WIFI LOST";
    case POWER_LINK_WIFI_CONNECTING:
        return "CONNECTING";
    case POWER_LINK_BOOT:
    default:
        return "STARTING";
    }
}

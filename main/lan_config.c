#include "lan_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web_form.h"

static const char *TAG = "lan_config";

#define FORM_MAX 1024
#define PAGE_MAX 4600

static app_config_t s_cfg;
static char s_page[PAGE_MAX];

static const char PAGE_BAD[] =
    "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<title>参数不完整</title></head><body style='background:#0b0f14;color:#e8f0f7;"
    "font-family:-apple-system,sans-serif;padding:40px 20px'>"
    "<h1>参数不完整</h1><p>请返回上一页检查 Wi-Fi 名称和所选模式对应的地址。</p>"
    "<p><a style='color:#ffb020' href='/'>返回</a></p></body></html>";

static const char PAGE_SAVED[] =
    "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<title>已保存</title><style>body{background:#0b0f14;color:#e8f0f7;"
    "font-family:-apple-system,sans-serif;text-align:center;padding:60px 24px}"
    "h1{font-size:20px}p{color:#8ca0b3;font-size:14px}</style></head><body>"
    "<h1>配置已保存</h1><p>设备正在重启，约 2 秒后以新配置运行。</p></body></html>";

static void copy_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    const size_t n = src ? strlen(src) : 0;
    const size_t copy = n >= dst_len ? dst_len - 1 : n;
    if (copy > 0) memcpy(dst, src, copy);
    dst[copy] = '\0';
}

static void build_page(httpd_req_t *req) {
    char host_hdr[48] = "本机地址";
    size_t host_len = sizeof(host_hdr);
    if (httpd_req_get_hdr_value_str(req, "Host", host_hdr, host_len) != ESP_OK) {
        copy_str(host_hdr, sizeof(host_hdr), "本机地址");
    }

    char ssid[APP_CONFIG_SSID_MAX * 3];
    web_html_escape(s_cfg.ssid, ssid, sizeof(ssid));
    const bool has_mcp = s_cfg.mcp_url[0] != '\0';


    snprintf(s_page, sizeof(s_page),
             "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>AI Passport 设置</title><style>"
             "*{box-sizing:border-box}body{margin:0;background:#0b0f14;color:#e8f0f7;"
             "font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif;padding:24px 18px}"
             "h1{font-size:20px;margin:0 0 4px}p.sub{color:#8ca0b3;font-size:13px;margin:0 0 20px}"
             "label{display:block;font-size:13px;color:#8ca0b3;margin:16px 0 6px}"
             "input,select{width:100%%;padding:12px;border-radius:10px;border:1px solid #24313f;"
             "background:#161e27;color:#e8f0f7;font-size:16px}"
             "button{width:100%%;margin-top:26px;padding:14px;border:0;border-radius:12px;"
             "background:#ffb020;color:#1a1200;font-size:16px;font-weight:600}"
             "small{display:block;color:#5f7385;font-size:12px;line-height:1.6;margin-top:22px}"
             ".meta{color:#63e6a0;font-size:13px}</style></head><body>"
             "<h1>AI Passport 设置</h1>"
             "<p class='sub'>当前地址 <span class='meta'>%s</span> · 固件 %s</p>"
             "<form method='post' action='/save'>"
             "<label>Wi-Fi 名称 (SSID)</label>"
             "<input name='ssid' maxlength='32' value='%s' required>"
             "<label>Wi-Fi 密码（留空保持不变）</label>"
             "<input name='password' type='password' maxlength='64' placeholder='不修改就留空'>"
             "<label>小电拼 MCP 地址（%s）</label>"
             "<input name='mcp' maxlength='190' value='' placeholder='留空保持不变；填新的会覆盖'>"
             "<button type='submit'>保存并重启</button></form>"
             "<small>设备仅通过云端 MCP 连接小电拼。"
             "MCP 地址含设备 token，这里不回显。保存后设备约 2 秒后重启。</small></body></html>",
             host_hdr, esp_app_get_description()->version, ssid,
             has_mcp ? "已配置，留空保持不变" : "未配置");
}

static esp_err_t root_handler(httpd_req_t *req) {
    build_page(req);
    return web_send_html(req, s_page);
}

static esp_err_t save_handler(httpd_req_t *req) {
    static char body[FORM_MAX + 1];
    static char raw_ssid[APP_CONFIG_SSID_MAX];
    static char raw_mcp[APP_CONFIG_MCP_MAX * 2];
    static char pass[APP_CONFIG_PASS_MAX];

    if (req->content_len <= 0 || req->content_len > FORM_MAX) return web_send_html(req, PAGE_BAD);

    size_t received = 0;
    while (received < (size_t)req->content_len) {
        const int r = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return web_send_html(req, PAGE_BAD);
        }
        received += (size_t)r;
    }
    body[received] = '\0';

    app_config_t next;
    memset(&next, 0, sizeof(next));

    if (!web_form_get(body, "ssid", raw_ssid, sizeof(raw_ssid)) || raw_ssid[0] == '\0') {
        return web_send_html(req, PAGE_BAD);
    }

    next.mode = APP_LINK_CLOUD;

    // MCP 地址不回显：留空表示保持原值。
    raw_mcp[0] = '\0';
    (void)web_form_get(body, "mcp", raw_mcp, sizeof(raw_mcp));
    if (raw_mcp[0] != '\0') {
        app_config_normalize_mcp(raw_mcp, next.mcp_url, sizeof(next.mcp_url));
    } else {
        copy_str(next.mcp_url, sizeof(next.mcp_url), s_cfg.mcp_url);
    }
    if (next.mcp_url[0] == '\0') return web_send_html(req, PAGE_BAD);

    // 密码留空表示沿用旧值。
    if (web_form_get(body, "password", pass, sizeof(pass)) && pass[0] != '\0') {
        copy_str(next.password, sizeof(next.password), pass);
    } else {
        copy_str(next.password, sizeof(next.password), s_cfg.password);
    }

    copy_str(next.ssid, sizeof(next.ssid), raw_ssid);

    const esp_err_t err = app_config_save(&next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存配置失败: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return web_send_html(req, PAGE_BAD);
    }

    ESP_LOGI(TAG, "运行时配置已保存，即将重启");
    const esp_err_t sent = web_send_html(req, PAGE_SAVED);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return sent;
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t lan_config_start(const app_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.max_uri_handlers = 3;
    httpd_cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &httpd_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置页启动失败: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);

    ESP_LOGI(TAG, "运行时配置页已启动: http://<设备IP>/");
    return ESP_OK;
}

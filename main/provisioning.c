#include "provisioning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "web_form.h"
#include "gbk_table.h"

static const char *TAG = "provisioning";

// 中文 SSID/密码按 %XX 编码后最多膨胀三倍，1024 字节足够覆盖 32+64+63 的输入。
#define FORM_MAX 1024
#define AP_SCAN_MAX 12
#define SCAN_JSON_MAX 1024

static SemaphoreHandle_t s_scan_lock;
static esp_netif_t *s_sta_netif;
static char s_scan_json[SCAN_JSON_MAX];

static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>AI Passport · 连接小电拼</title><style>"
    "*{box-sizing:border-box}html,body{max-width:100%;overflow-x:hidden}body{margin:0;background:#0b0f14;color:#e8f0f7;"
    "font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif;padding:24px 18px}"
    "h1{font-size:20px;margin:0 0 4px}p.sub{color:#8ca0b3;font-size:13px;margin:0 0 18px}"
    "label{display:block;font-size:13px;color:#8ca0b3;margin:16px 0 6px}"
    "input,select{width:100%;padding:12px;border-radius:10px;border:1px solid #24313f;"
    "background:#161e27;color:#e8f0f7;font-size:16px}"
    "input[type=checkbox]{width:auto;margin:0 6px 0 0;vertical-align:middle}"
    ".chk{display:flex;align-items:center;gap:0;margin:8px 0 0;font-size:13px;color:#8ca0b3}"
    "button{width:100%;margin-top:26px;padding:14px;border:0;border-radius:12px;"
    "background:#ffb020;color:#1a1200;font-size:16px;font-weight:600}"
    "small{display:block;color:#5f7385;font-size:12px;line-height:1.6;margin-top:22px}"
    ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#63e6a0;"
    "margin-right:6px;vertical-align:middle}"
    "#nets{display:flex;flex-direction:column;gap:8px;margin:6px 0 4px}"
    ".net{display:flex;justify-content:space-between;align-items:center;gap:12px;min-width:0;max-width:100%;margin:0;padding:12px 14px;"
    "border-radius:10px;background:#161e27;border:1px solid #24313f;color:#e8f0f7;font-size:15px;"
    "font-weight:400;text-align:left}"
    ".net.on{border-color:#ffb020;background:#241d10}"
    ".net .rs{color:#8ca0b3;font-size:13px;white-space:nowrap}.net span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}"
    ".mut{color:#8ca0b3;font-size:13px;margin:8px 0}"
    "#rescan{width:auto;margin:0;padding:8px 14px;background:#1d2833;color:#cfe0ee;font-size:13px}"
    "</style></head><body><h1><span class='dot'></span>连接小电拼</h1>"
    "<p class='sub'>选择家里的 Wi-Fi，填入小电拼 MCP 地址；保存后设备联网即可远程查看功率。</p>"
    "<label>附近的 Wi-Fi <button id='rescan' type='button' onclick='doScan()'>重新扫描</button></label>"
    "<div id='nets'><p class='mut'>正在扫描…</p></div>"
    "<form method='post' action='/save'>"
    "<label>Wi-Fi 名称 (SSID)</label>"
    "<input id='ssid' name='ssid' maxlength='32' placeholder='点上方列表选择，或手动输入' required>"
    "<label>Wi-Fi 密码</label>"
    "<input id='pw' name='password' type='password' maxlength='64' placeholder='留空表示开放网络'"
    " autocomplete='off' autocapitalize='off' spellcheck='false'>"
    "<label class='chk'><input type='checkbox'"
    " onclick=\"var p=document.getElementById('pw');p.type=this.checked?'text':'password'\">"
    "显示密码（核对大小写和特殊字符）</label>"
    "<label>小电拼 MCP 地址</label>"
    "<input name='mcp' maxlength='190' placeholder='https://mcp.thecandysign.com/&lt;psn&gt;/&lt;token&gt;/sse' required>"
    "<button type='submit'>保存并重启</button></form>"
    "<small>MCP 地址在官方 App 的开放接口 / AI 接入页复制，"
    "整条链接（含 token）都要填进来。token 相当于设备密码，只保存在设备本地 NVS，不会上传或打印。</small>"
    "<script>"
    "function setSsid(name){document.getElementById('ssid').value=name;document.getElementById('pw').focus();}"
    "async function doScan(){const box=document.getElementById('nets');box.innerHTML='<p class=mut>正在扫描…</p>';"
    "try{const r=await fetch('/scan',{cache:'no-store'});const arr=await r.json();"
    "if(!arr.length){box.innerHTML='<p class=mut>没有发现网络，点“重新扫描”再试。</p>';return;}"
    "box.innerHTML='';arr.forEach(function(n){var b=document.createElement('button');b.type='button';b.className='net';"
    "var s=document.createElement('span');s.textContent=n.ssid;var r2=document.createElement('span');r2.className='rs';"
    "r2.textContent=n.rssi+' dBm'+(n.auth?' · 加密':'');b.appendChild(s);b.appendChild(r2);"
    "b.onclick=function(){document.querySelectorAll('.net').forEach(function(x){x.classList.remove('on');});"
    "b.classList.add('on');setSsid(n.ssid);};box.appendChild(b);});}"
    "catch(e){box.innerHTML='<p class=mut>扫描失败，请重试。</p>';}}"
"window.addEventListener('load',doScan);"
    "</script></body></html>";

static const char PAGE_SAVED[] =
    "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>已保存</title><style>body{background:#0b0f14;color:#e8f0f7;"
    "font-family:-apple-system,'PingFang SC',sans-serif;text-align:center;padding:60px 24px}"
    "h1{font-size:20px}p{color:#8ca0b3;font-size:14px}</style></head><body>"
    "<h1>配置已保存</h1><p>设备正在重启，稍后屏幕会显示实时功率。</p></body></html>";

static const char PAGE_BAD[] =
    "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<title>参数不完整</title></head><body>"
    "<h1>参数不完整</h1><p>请返回上一页选择 Wi-Fi 并填写 MCP 地址。</p></body></html>";

static esp_err_t root_handler(httpd_req_t *req) {
    return web_send_html(req, PAGE_HEAD);
}

static void copy_field(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    const size_t n = src ? strlen(src) : 0;
    const size_t copy = n >= dst_len ? dst_len - 1 : n;
    if (copy > 0) memcpy(dst, src, copy);
    dst[copy] = '\0';
}

// ---------------------------------------------------------------- Wi-Fi 扫描


static bool utf8_valid(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        const uint8_t c = s[i];
        int len = 0;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return false;
        if (i + (size_t)len > n) return false;
        for (int k = 1; k < len; k++) {
            if ((s[i + (size_t)k] & 0xC0) != 0x80) return false;
        }
        i += (size_t)len;
    }
    return true;
}

static uint16_t gbk_lookup(uint16_t code) {
    size_t lo = 0, hi = GBK_TABLE_COUNT;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (GBK_TABLE[mid].gbk == code) return GBK_TABLE[mid].uni;
        if (GBK_TABLE[mid].gbk < code) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

// SSID 常见 UTF-8 与 GBK 两种编码：优先按 UTF-8，非法时按 GBK 转码，避免中文乱码。
static size_t ssid_to_utf8(const uint8_t *src, size_t n, char *out, size_t cap) {
    if (n > cap - 1) n = cap - 1;
    if (utf8_valid(src, n)) {
        memcpy(out, src, n);
        out[n] = 0;
        return n;
    }
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap;) {
        const uint8_t c = src[i];
        if (c < 0x80) { out[o++] = (char)c; i++; continue; }
        if (i + 1 >= n) break;
        const uint16_t cp = gbk_lookup((uint16_t)((c << 8) | src[i + 1]));
        i += 2;
        if (cp == 0) { out[o++] = '?'; continue; }
        if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

static size_t json_escape(char *dst, size_t cap, const char *src, size_t len) {
    size_t o = 0;
    for (size_t i = 0; i < len && src[i] != '\0' && o + 1 < cap; i++) {
        const unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) break;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 6 >= cap) break;
            const int n = snprintf(dst + o, cap - o, "\\u%04x", c);
            if (n <= 0) break;
            o += (size_t)n;
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

static esp_err_t scan_handler(httpd_req_t *req) {
    if (s_scan_lock && xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(12000)) != pdPASS) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "[]");
    }

    wifi_scan_config_t sc = {0};
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 120;
    sc.scan_time.active.max = 300;

    uint16_t num = 0;
    wifi_ap_record_t *recs = calloc(AP_SCAN_MAX, sizeof(wifi_ap_record_t));
    if (recs && esp_wifi_scan_start(&sc, true) == ESP_OK) {
        if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) num = 0;
        if (num > AP_SCAN_MAX) num = AP_SCAN_MAX;
        if (num > 0 && esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) num = 0;
    } else {
        num = 0;
    }

    size_t o = 0;
    int written = snprintf(s_scan_json, sizeof(s_scan_json), "[");
    if (written > 0) o = (size_t)written;

    bool first = true;
    for (uint16_t i = 0; i < num && recs; i++) {
        size_t n = 0;
        while (n < 32 && recs[i].ssid[n] != 0) n++;
        if (n == 0) continue;

        char raw[96];
        const size_t raw_len = ssid_to_utf8(recs[i].ssid, n, raw, sizeof(raw));

        char esc[160];
        json_escape(esc, sizeof(esc), raw, raw_len);
        written = snprintf(s_scan_json + o, sizeof(s_scan_json) - o,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                           first ? "" : ",", esc,
                           recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN ? 1 : 0);
        if (written <= 0 || (size_t)written >= sizeof(s_scan_json) - o) break;
        o += (size_t)written;
        first = false;
    }
    snprintf(s_scan_json + o, sizeof(s_scan_json) - o, "]");
    free(recs);
    if (s_scan_lock) xSemaphoreGive(s_scan_lock);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, s_scan_json);
}

// 让手机系统把本热点识别为“需登录网络”，从而自动弹出配置页。
static void dns_task(void *arg) {
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "DNS socket 创建失败");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "DNS 端口绑定失败");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t in[512];
    uint8_t out[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&from, &from_len);
        if (n < 12) continue;

        int qlen = 12;
        while (qlen < n && in[qlen] != 0) qlen += 1 + in[qlen];
        qlen += 1;
        if (qlen + 4 > n) continue;
        const uint16_t qtype = (uint16_t)((in[qlen] << 8) | in[qlen + 1]);
        qlen += 4;

        memcpy(out, in, (size_t)qlen);
        out[2] = (uint8_t)(in[2] | 0x80);  // QR
        out[3] = (uint8_t)(in[3] | 0x80);  // RA
        out[6] = 0;
        out[7] = (qtype == 1) ? 1 : 0;     // ANCOUNT
        out[8] = out[9] = out[10] = out[11] = 0;

        int len = qlen;
        if (qtype == 1 && len + 16 <= (int)sizeof(out)) {
            uint8_t *p = out + len;
            *p++ = 0xC0; *p++ = 0x0C;      // 指向问题里的域名
            *p++ = 0x00; *p++ = 0x01;      // A
            *p++ = 0x00; *p++ = 0x01;      // IN
            *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C;  // TTL 60
            *p++ = 0x00; *p++ = 0x04;
            *p++ = 192; *p++ = 168; *p++ = 4; *p++ = 1;
            len += 16;
        }

        sendto(sock, out, (size_t)len, 0, (struct sockaddr *)&from, from_len);
    }
}


static esp_err_t captive_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PROVISIONING_AP_IP "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t save_handler(httpd_req_t *req) {
    static char body[FORM_MAX + 1];
    static app_config_t cfg;
    static char raw_ssid[APP_CONFIG_SSID_MAX];
    static char raw_mcp[APP_CONFIG_MCP_MAX * 2];

    if (req->content_len <= 0 || req->content_len > FORM_MAX) {
        return web_send_html(req, PAGE_BAD);
    }

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

    memset(&cfg, 0, sizeof(cfg));
    if (!web_form_get(body, "ssid", raw_ssid, sizeof(raw_ssid))) {
        return web_send_html(req, PAGE_BAD);
    }

    char pass[APP_CONFIG_PASS_MAX] = {0};
    (void)web_form_get(body, "password", pass, sizeof(pass));

    cfg.mode = APP_LINK_CLOUD;

    if (!web_form_get(body, "mcp", raw_mcp, sizeof(raw_mcp))) return web_send_html(req, PAGE_BAD);
    app_config_normalize_mcp(raw_mcp, cfg.mcp_url, sizeof(cfg.mcp_url));
    if (cfg.mcp_url[0] == '\0') return web_send_html(req, PAGE_BAD);

    if (raw_ssid[0] == '\0') return web_send_html(req, PAGE_BAD);

    copy_field(cfg.ssid, sizeof(cfg.ssid), raw_ssid);
    copy_field(cfg.password, sizeof(cfg.password), pass);

    const esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存配置失败: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return web_send_html(req, PAGE_BAD);
    }

    // 不打印 MCP 地址：其中包含设备 token。
    ESP_LOGI(TAG, "配置已保存: ssid=%s", cfg.ssid);
    const esp_err_t sent = web_send_html(req, PAGE_SAVED);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return sent;
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PROVISIONING_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t provisioning_start(char *out_ssid, size_t out_ssid_len) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    char ssid[32];
    snprintf(ssid, sizeof(ssid), "AIPassport-%02X%02X", mac[4], mac[5]);
    if (out_ssid && out_ssid_len) copy_field(out_ssid, out_ssid_len, ssid);

    wifi_config_t ap_cfg = {0};
    memcpy(ap_cfg.ap.ssid, ssid, strlen(ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 2;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.pmf_cfg.required = false;

    // APSTA：保持热点可用，同时允许扫描附近的 Wi-Fi。
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    s_scan_lock = xSemaphoreCreateMutex();
    static bool dns_started;
    if (!dns_started) {
        dns_started = true;
        xTaskCreate(dns_task, "prov_dns", 3072, NULL, 4, NULL);
    }

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.max_uri_handlers = 12;
    httpd_cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    err = httpd_start(&server, &httpd_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler, .user_ctx = NULL };
    const httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &scan);

    static const char *const captive_uris[] = {
        "/generate_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt",
        "/redirect", "/canonical.html", "/success.txt",
    };
    for (size_t i = 0; i < sizeof(captive_uris) / sizeof(captive_uris[0]); i++) {
        const httpd_uri_t cap = { .uri = captive_uris[i], .method = HTTP_GET, .handler = captive_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &cap);
    }

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);

    ESP_LOGI(TAG, "配置热点已就绪: %s -> http://%s", ssid, PROVISIONING_AP_IP);
    return ESP_OK;
}

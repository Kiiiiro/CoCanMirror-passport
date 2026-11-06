#include "app_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_config";
static const char *NVS_NAMESPACE = "cocan_app";
// 设备状态单独存放：重新配网只清 cocan_app，开关状态和亮度不会被一起抹掉。
static const char *NVS_STATE_NAMESPACE = "cocan_state";

static esp_err_t read_string(nvs_handle_t handle, const char *key, char *dst, size_t dst_len) {
    size_t size = dst_len;
    esp_err_t err = nvs_get_str(handle, key, dst, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        dst[0] = '\0';
        return err;
    }
    if (err != ESP_OK) dst[0] = '\0';
    return err;
}

esp_err_t app_config_load(app_config_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    uint8_t mode = APP_LINK_LAN;
    (void)nvs_get_u8(handle, "mode", &mode);
    if (mode > APP_LINK_AUTO) mode = APP_LINK_LAN;
    out->mode = (app_link_mode_t)mode;

    (void)read_string(handle, "ssid", out->ssid, sizeof(out->ssid));
    (void)read_string(handle, "pass", out->password, sizeof(out->password));
    (void)read_string(handle, "host", out->host, sizeof(out->host));
    (void)read_string(handle, "mcp", out->mcp_url, sizeof(out->mcp_url));
    nvs_close(handle);

    if (out->ssid[0] == '\0') return ESP_ERR_NOT_FOUND;
    // 云端必须有 MCP；局域网/自动允许地址为空，开机后会自动查找小电拼。
    // 2.3 起只支持云端 MCP；没有 MCP 地址视为未配置，进入配网。
    out->mode = APP_LINK_CLOUD;
    if (out->mcp_url[0] == '\0') return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg) {
    if (!cfg || cfg->ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    // 云端必须配置 MCP；局域网/自动允许暂时缺少地址，由界面提示用户补全。
    if (cfg->mcp_url[0] == '\0') return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, "mode", (uint8_t)APP_LINK_CLOUD);
    if (err == ESP_OK) err = nvs_set_str(handle, "ssid", cfg->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "pass", cfg->password);
    if (err == ESP_OK) err = nvs_set_str(handle, "host", cfg->host);
    if (err == ESP_OK) err = nvs_set_str(handle, "mcp", cfg->mcp_url);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    // 不打印 MCP 地址：它带有设备 token。
    if (err != ESP_OK) ESP_LOGE(TAG, "保存配置失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t app_config_clear(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

uint16_t app_config_get_port_mask(void) {
    uint16_t mask = 0x003F;  // 默认全部开启
    nvs_handle_t handle;

    if (nvs_open(NVS_STATE_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint16_t stored = 0;
        if (nvs_get_u16(handle, "port_on", &stored) == ESP_OK) {
            nvs_close(handle);
            return stored;
        }
        nvs_close(handle);
    }

    // 兼容旧固件：状态原先存在 cocan_app 里，读到后迁移到独立命名空间。
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint16_t stored = 0;
        if (nvs_get_u16(handle, "port_on", &stored) == ESP_OK) {
            mask = stored;
            (void)app_config_set_port_mask(mask);
            ESP_LOGI(TAG, "端口开关状态已迁移到独立命名空间: 0x%04X", (unsigned)mask);
        }
        nvs_close(handle);
    }
    return mask;
}

esp_err_t app_config_set_port_mask(uint16_t mask) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(handle, "port_on", mask);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

void app_config_normalize_host(const char *input, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!input) return;

    const char *p = input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;

    size_t n = 0;
    while (*p && *p != '/' && *p != '?' && *p != '#' && !isspace((unsigned char)*p)) {
        if (n + 1 >= out_len) break;
        out[n++] = *p++;
    }
    while (n > 0 && out[n - 1] == '/') n--;
    out[n] = '\0';
}

void app_config_normalize_mcp(const char *input, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!input) return;

    const char *p = input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "https://", 8) != 0) return;  // 云端 MCP 必须走 TLS

    // 保留 "https://" 前缀，只裁掉路径末尾的 /sse、/mcp 再补回 /mcp。
    size_t n = 0;
    while (*p && !isspace((unsigned char)*p)) {
        if (n + 1 >= out_len) {
            out[0] = '\0';
            return;
        }
        out[n++] = *p++;
    }
    while (n > 8 && out[n - 1] == '/') n--;
    out[n] = '\0';

    if (n >= 4 && (strcmp(out + n - 4, "/sse") == 0 || strcmp(out + n - 4, "/mcp") == 0)) {
        n -= 4;
        out[n] = '\0';
    }
    if (n < 12 || n + 5 > out_len) {  // 至少 https:// + 主机名
        out[0] = '\0';
        return;
    }
    memcpy(out + n, "/mcp", 4);
    out[n + 4] = '\0';
}

void app_config_url_host(const char *url, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!url) return;

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    size_t n = 0;
    while (*p && *p != '/' && *p != ':' && n + 1 < out_len) out[n++] = *p++;
    out[n] = '\0';
}


// 屏幕亮度档位：20 / 50 / 80 / 100（百分比）。
static const uint8_t s_brightness_levels[] = {20, 50, 80, 100};

uint8_t app_config_get_brightness(void) {
    nvs_handle_t handle;
    uint8_t value = 100;
    bool found = false;

    if (nvs_open(NVS_STATE_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        found = nvs_get_u8(handle, "bright", &value) == ESP_OK;
        nvs_close(handle);
    }

    // 兼容旧固件：亮度原先存在 cocan_app 里。
    if (!found && nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        found = nvs_get_u8(handle, "bright", &value) == ESP_OK;
        nvs_close(handle);
        if (found) (void)app_config_set_brightness(value);
    }

    if (!found) return 100;
    for (size_t i = 0; i < sizeof(s_brightness_levels); i++) {
        if (s_brightness_levels[i] == value) return value;
    }
    return 100;
}

esp_err_t app_config_set_brightness(uint8_t percent) {
    bool valid = false;
    for (size_t i = 0; i < sizeof(s_brightness_levels); i++) {
        if (s_brightness_levels[i] == percent) valid = true;
    }
    if (!valid) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, "bright", percent);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

uint8_t app_config_get_lang(void) {
    nvs_handle_t handle;
    uint8_t value = 1;
    if (nvs_open(NVS_STATE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 1;
    const esp_err_t err = nvs_get_u8(handle, "lang", &value);
    nvs_close(handle);
    return err == ESP_OK ? (value ? 1 : 0) : 1;
}

esp_err_t app_config_set_lang(uint8_t zh) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "lang", zh ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}


uint16_t app_config_get_poll_ms(void) {
    nvs_handle_t handle;
    uint16_t value = 1000;
    if (nvs_open(NVS_STATE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 1000;
    const esp_err_t err = nvs_get_u16(handle, "poll_ms", &value);
    nvs_close(handle);
    if (err != ESP_OK) return 1000;
    if (value != 500 && value != 1000 && value != 3000) return 1000;
    return value;
}

esp_err_t app_config_set_poll_ms(uint16_t ms) {
    if (ms != 500 && ms != 1000 && ms != 3000) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(handle, "poll_ms", ms);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

uint8_t app_config_get_idle_pause(void) {
    nvs_handle_t handle;
    uint8_t value = 1;
    if (nvs_open(NVS_STATE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 1;
    const esp_err_t err = nvs_get_u8(handle, "idle_pause", &value);
    nvs_close(handle);
    return err == ESP_OK ? (value ? 1 : 0) : 1;
}

esp_err_t app_config_set_idle_pause(uint8_t pause) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "idle_pause", pause ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}


uint32_t app_config_get_screen_off_ms(void) {
    nvs_handle_t handle;
    uint32_t value = 300000;
    if (nvs_open(NVS_STATE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 300000;
    const esp_err_t err = nvs_get_u32(handle, "off_ms", &value);
    nvs_close(handle);
    if (err != ESP_OK) return 300000;
    if (value != 0 && value != 60000 && value != 180000 && value != 300000) return 300000;
    return value;
}

esp_err_t app_config_set_screen_off_ms(uint32_t ms) {
    if (ms != 0 && ms != 60000 && ms != 180000 && ms != 300000) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, "off_ms", ms);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t app_config_factory_reset(void) {
    return nvs_flash_erase();
}

// main/app_config.h —— 本应用的持久化配置(Wi-Fi、局域网地址或云端 MCP)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_CONFIG_SSID_MAX 33
#define APP_CONFIG_PASS_MAX 65
#define APP_CONFIG_HOST_MAX 64
#define APP_CONFIG_MCP_MAX 192

typedef enum {
    APP_LINK_LAN = 0,    // 同一局域网，轮询 http://<host>/metrics
    APP_LINK_CLOUD = 1,  // 任意网络，走小电拼云端 MCP (Streamable HTTP)
    APP_LINK_AUTO = 2,   // 优先局域网，不可达时自动切云端
} app_link_mode_t;

typedef struct {
    app_link_mode_t mode;
    char ssid[APP_CONFIG_SSID_MAX];
    char password[APP_CONFIG_PASS_MAX];
    char host[APP_CONFIG_HOST_MAX];    // LAN 模式：充电器 IPv4 / 主机名
    char mcp_url[APP_CONFIG_MCP_MAX];  // Cloud 模式：https://.../<psn>/<token>/mcp
} app_config_t;

// 读取 NVS 配置。缺少 Wi-Fi 或所选模式的目标地址时返回 ESP_ERR_NOT_FOUND。
esp_err_t app_config_load(app_config_t *out);

// 写入 NVS 配置。ssid 与所选模式的目标为空时返回 ESP_ERR_INVALID_ARG。
esp_err_t app_config_save(const app_config_t *cfg);

// 删除本应用的 NVS 命名空间(重新配网时使用)。
esp_err_t app_config_clear(void);

// 把用户输入整理成可放进 URL 的主机部分：去掉协议前缀、路径、空白与末尾斜杠。
// 例："http://192.168.1.50/" -> "192.168.1.50"。
void app_config_normalize_host(const char *input, char *out, size_t out_len);

// 把用户粘贴的 MCP 地址整理成 Streamable HTTP 端点：
//   接受 .../<psn>/<token>、.../sse、.../mcp，统一输出 https://.../mcp。
// 非 https 或长度不足时输出空串。
void app_config_normalize_mcp(const char *input, char *out, size_t out_len);

// 取 URL 的主机名(不含协议与路径)，用于设置页显示，避免把 token 亮在屏幕上。
void app_config_url_host(const char *url, char *out, size_t out_len);

// 端口输出状态的持久化(bit0 = 端口1 ... bit5 = 端口6；1=开)。
// 设备没有查询开关状态的接口，重启后按上次命令恢复显示。
uint16_t app_config_get_port_mask(void);
esp_err_t app_config_set_port_mask(uint16_t mask);

// 屏幕亮度档位：20 / 50 / 80 / 100（百分比）。默认 100。
uint8_t app_config_get_brightness(void);
esp_err_t app_config_set_brightness(uint8_t percent);

// UI 语言：1 = 中文，0 = English。默认中文。
uint8_t app_config_get_lang(void);
esp_err_t app_config_set_lang(uint8_t zh);

// 局域网轮询间隔(ms)：500 / 1000 / 3000，默认 1000。
uint16_t app_config_get_poll_ms(void);
esp_err_t app_config_set_poll_ms(uint16_t ms);

// 息屏是否暂停轮询：1 = 暂停（默认），0 = 继续低频轮询。
uint8_t app_config_get_idle_pause(void);
esp_err_t app_config_set_idle_pause(uint8_t pause);

// 自动息屏时间(ms)：60000 / 180000 / 300000 / 0(常亮)，默认 300000。
uint32_t app_config_get_screen_off_ms(void);
esp_err_t app_config_set_screen_off_ms(uint32_t ms);

// 恢复出厂：清空全部 NVS（含 Wi-Fi、状态、语言），之后需要重新配网。
esp_err_t app_config_factory_reset(void);

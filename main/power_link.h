// main/power_link.h —— Wi-Fi 连接小电拼，轮询功率并通过云端 MCP 控制端口开关。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "power_metrics.h"

#define POWER_HISTORY_LEN 120

typedef enum {
    POWER_LINK_BOOT = 0,
    POWER_LINK_WIFI_CONNECTING,
    POWER_LINK_WIFI_FAILED,
    POWER_LINK_HTTP_ERROR,
    POWER_LINK_LIVE,
} power_link_state_t;

typedef enum {
    POWER_CTL_IDLE = 0,
    POWER_CTL_BUSY,
    POWER_CTL_OK,
    POWER_CTL_FAIL,
} power_ctl_state_t;

typedef struct {
    bool valid;
    bool tried;  // 已经请求过（用于区分“还没取”和“设备没有该口数据”）
    uint8_t port;  // 1-based
    bool pps;
    bool epr;
    bool avs;
    bool emarker;
    char protocol[40];  // 该口当前快充协议（随 PD 信息一起拉取）
    char cable_max_v[16];  // 可能是 "20V"，也可能是 "unknown(3)"，需要容纳原始枚举
    char cable_max_a[16];
    char cable_speed[20];
    char cable_element[12];  // passive / active
    uint16_t cable_vid;
    uint16_t cable_pid;
    uint32_t op_mv;
    uint32_t op_ma;
} power_pd_info_t;

typedef struct {
    power_snapshot_t metrics;
    power_link_state_t state;
    bool via_cloud;  // 当前数据来自云端 MCP（AUTO 模式下会切换）
    int http_status;
    bool rssi_valid;
    int8_t rssi;
    char ip[16];
    uint32_t poll_ok;
    uint32_t poll_fail;
    int64_t last_ok_ms;
    uint32_t seq;

    // 端口输出开关状态（只能通过云端 MCP 读取/控制；局域网轮询不会覆盖）。
    bool port_on[POWER_METRICS_MAX_PORTS];
    bool port_on_valid[POWER_METRICS_MAX_PORTS];
    power_ctl_state_t ctl_state;
    int ctl_port;   // 1-based
    bool ctl_on;    // 请求的目标状态
    int ctl_error;  // HTTP 状态码或 -1

    // 详情页的 PD / 线材信息（按需从云端 MCP 拉取）。
    power_pd_info_t pd[POWER_METRICS_MAX_PORTS];

    // 每次成功轮询写入一条的环形历史，单位毫瓦(仅保留最近 POWER_HISTORY_LEN 次采样)。
    uint32_t total_history[POWER_HISTORY_LEN];
    uint32_t port_history[POWER_METRICS_MAX_PORTS][POWER_HISTORY_LEN];
    uint8_t history_len;
    uint8_t history_head;
} power_link_view_t;

// 启动后台任务：连接 Wi-Fi、轮询功率，并处理端口开关请求。
esp_err_t power_link_start(const app_config_t *cfg);

// 取最近一次快照(线程安全拷贝)。
void power_link_get(power_link_view_t *out);

// 请求把某个端口(1-based)打开/关闭。走云端 MCP，需要已配置 MCP 地址。
esp_err_t power_link_control(int port, bool on);

// 一次 MCP 调用切换全部接口。
esp_err_t power_link_control_all(bool on);

// 请求刷新某端口的 PD / 线材信息（进入详情页时调用；需要 MCP 地址）。
esp_err_t power_link_request_pd(int port);

void power_link_log_tools(void);

void power_link_request_tool_list(void);

void power_link_log_details(void);

// 运行时调整局域网轮询间隔(ms)与息屏暂停策略。
void power_link_set_poll_ms(uint16_t ms);
void power_link_set_idle_pause(bool pause);

// 扫描 Wi-Fi 期间暂停 HTTP 轮询，避免射频争用导致驱动卡死。
void power_link_set_scanning(bool scanning);

// 首次未填充电器地址时，后台自动在局域网里查找。
typedef enum {
    POWER_DISCOVER_IDLE = 0,
    POWER_DISCOVER_RUNNING,
    POWER_DISCOVER_FOUND,
    POWER_DISCOVER_FAILED,
} power_discover_t;

power_discover_t power_link_discover_state(void);

// 息屏省电：暂停期间把轮询降到 10s/60s，并把 Wi-Fi 切到 MAX_MODEM；
// 恢复时立即触发一次刷新并切回低延迟模式。
void power_link_set_idle(bool idle);

// 状态英文短语，供 UI 直接显示。
const char *power_link_state_text(power_link_state_t state);

// main/power_metrics.h —— 小电拼(CoCan)本地 /metrics 的解析与功率计算。
//
// 该文件只依赖 libc，不依赖 ESP-IDF/LVGL，便于在主机上做单元测试。
// 数据来源：小电拼固件本地 Prometheus 文本端点 http://<device>/metrics，
// 例如：
//   ionbridge_port_voltage{id="0"} 5000
//   ionbridge_port_current{id="0"} 2500
//   ionbridge_uptime_seconds 3600
//   ionbridge_wifi_signal{ssid="...",channel="6"} -50
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 小电拼 CP02 为 5 个接口(A1/C1/C2/C3/C4)；留一个余量以兼容后续型号。
#define POWER_METRICS_MAX_PORTS 6

typedef struct {
    bool has_voltage;
    bool has_current;
    uint32_t voltage_mv;
    uint32_t current_ma;
    char protocol[40];     // 快充协议名，仅云端 MCP 提供；局域网模式为空
    char temperature[12];  // 端口温度等级(cool/moderate/warm)，仅云端 MCP 提供
} power_port_metrics_t;

typedef struct {
    power_port_metrics_t ports[POWER_METRICS_MAX_PORTS];
    int port_count;  // 解析到的最大接口号 + 1；没有接口数据时为 0
    bool uptime_valid;
    uint64_t uptime_s;
    bool rssi_valid;
    int32_t rssi_dbm;
} power_snapshot_t;

// 解析 Prometheus 文本。成功返回 0，并至少解析出一个接口/字段时返回正数接口数；
// 文本不可解析或没有任何已知指标时返回 -1。out 会先被清零。
int power_metrics_parse(const char *text, size_t len, power_snapshot_t *out);

// 接口功率，单位毫瓦。缺少电压或电流时返回 0。
uint32_t power_port_milliwatts(const power_port_metrics_t *port);

// 接口是否在供电：有电流即视为在工作；否则电压存在也算在位。
bool power_port_active(const power_port_metrics_t *port);

// 按 CP02 的显示约定格式化瓦数：<10W 两位小数，<100W 一位小数，其余取整。
// 例：5.67 / 45.8 / 128。缓冲区不足时安全截断。
void power_format_watts(uint32_t milliwatts, char *buf, size_t len);

// 格式化电压，例如 "20.1"（单位由调用方追加）。
void power_format_volts(uint32_t millivolts, char *buf, size_t len);

// 格式化电流，例如 "2.24"（单位 A，由调用方追加）。
void power_format_amps(uint32_t milliamps, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

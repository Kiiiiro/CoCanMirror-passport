// main/provisioning.h —— 首次配网/重新配网：SoftAP + 手机网页表单。
#pragma once

#include <stddef.h>

#include "esp_err.h"

#define PROVISIONING_AP_IP "192.168.4.1"

// 启动配置门户。成功后 out_ssid 返回实际热点名(如 "AIPassport-3F2A")。
// 保存配置后会重启设备进入监控模式。
esp_err_t provisioning_start(char *out_ssid, size_t out_ssid_len);

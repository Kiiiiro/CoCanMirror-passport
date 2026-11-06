// main/lan_config.h —— 运行时的局域网配置页(浏览器打开设备 IP 即可改配置)。
#pragma once

#include "app_config.h"
#include "esp_err.h"

// 在 80 端口启动配置页。只应在已配网(STA 已运行)后调用，避免和 SoftAP 首配页抢端口。
esp_err_t lan_config_start(const app_config_t *cfg);

// main/app_ui.h —— 运行界面：总览 / 单口详情 / 系统信息 / 配网提示。
#pragma once

#include "app_config.h"
#include "bsp_button.h"

// cfg 为 NULL 时表示尚未配网，界面只显示配网引导。
void app_ui_init(const app_config_t *cfg);

// 首次配网时显示热点与配置地址；监控模式下由 app_ui_show_overview 进入主界面。
void app_ui_show_provisioning(const char *ap_ssid);
void app_ui_show_overview(void);

// 由按键任务调用；内部自行加 LVGL 锁。
void app_ui_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev);

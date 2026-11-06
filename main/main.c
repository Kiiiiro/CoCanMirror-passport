// main/main.c —— 应用入口：小电拼实时功率监测。
//
// 启动流程：
//   1. 初始化屏幕/LVGL/按键；
//   2. NVS 里已有 Wi-Fi 与充电器地址 -> 连接并轮询 /metrics，进入总览界面；
//   3. 没有配置 -> 启动 SoftAP 配置门户，屏幕提示热点名与地址。
//
// 按键(应用自定义)：
//   总览   UP/DOWN 选择接口，OK 短按进入详情，OK 长按进入设置
//   详情   UP/DOWN 切换接口，OK 返回总览
//   设置   OK 返回总览，OK 长按弹出“清除配网”确认
#include "app_config.h"
#include "app_ui.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "lan_config.h"
#include "nvs_flash.h"
#include "power_link.h"
#include "provisioning.h"

static const char *TAG = "app";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static key_guard_t s_key_guard;

// 按键回调运行在共享的 esp_timer 任务上：只入队，立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // 深睡按键唤醒后，唤醒键在启动期间仍被按住；先消费守卫，避免释放事件被
    // 下面的就绪检查丢掉而让守卫一直拦到超时。
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手，按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t event = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &event, 0);
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t event;
    for (;;) {
        if (xQueueReceive(s_input_queue, &event, portMAX_DELAY) == pdTRUE) {
            app_ui_handle_key(event.btn, event.event);
        }
    }
}

static esp_err_t input_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "app_input", 4096, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return bsp_button_init(on_key, NULL);
}

static esp_err_t nvs_prepare(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void) {
    ESP_LOGI(TAG, "AI Passport · 小电拼功率监控启动");

    // 按键唤醒时，那颗键在整个启动期间还按着：先挡住它的动作，直到松手。
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), 10LL * 1000 * 1000);
        ESP_LOGI(TAG, "按键唤醒：先屏蔽按键动作，直到那颗键松手");
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，应用无法启动");
        return;
    }
    bsp_display_backlight(100);
    (void)bsp_battery_init();

    const esp_err_t input_err = input_init();
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(input_err));
    } else if (s_key_guard.armed) {
        // 唤醒键可能在按键驱动启动前就已松手：按真实电压同步一次，立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    s_input_ready = input_err == ESP_OK;

    esp_err_t nvs_err = nvs_prepare();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(nvs_err));
    }

    app_config_t cfg;
    const bool provisioned = nvs_err == ESP_OK && app_config_load(&cfg) == ESP_OK;

    if (provisioned) {
        ESP_LOGI(TAG, "使用已保存配置: ssid=%s host=%s", cfg.ssid, cfg.host);
        app_ui_init(&cfg);
        const esp_err_t err = power_link_start(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "网络任务启动失败: %s", esp_err_to_name(err));
        }
        // 已配网时提供运行时配置页：浏览器打开设备 IP 即可改配置，无需按键重配。
        (void)lan_config_start(&cfg);
        app_ui_show_overview();
        return;
    }

    ESP_LOGI(TAG, "没有可用配置，进入配网模式");
    app_ui_init(NULL);
    char ssid[32] = {0};
    const esp_err_t err = provisioning_start(ssid, sizeof(ssid));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配网门户启动失败: %s", esp_err_to_name(err));
        return;
    }
    app_ui_show_provisioning(ssid);
}

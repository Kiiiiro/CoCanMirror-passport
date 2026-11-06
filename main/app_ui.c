#include "app_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "lvgl.h"

LV_FONT_DECLARE(ui_font_14)
LV_FONT_DECLARE(ui_font_20)
#include "power_link.h"
#include "power_metrics.h"
#include "provisioning.h"

static const char *TAG = "app_ui";

// 配色对齐小电拼小程序：暖白背景、白色卡片、橙色强调。
#define COL_BG 0xF7F1E6
#define COL_CARD 0xFFFFFF
#define COL_CARD_SEL 0xFDECDD
#define COL_LINE 0xE7DED1
#define COL_TEXT 0x1E1B18
#define COL_MUTED 0x8A8178
#define COL_DIM 0xB5ABA0
#define COL_AMBER 0xF05A28
#define COL_CYAN 0xE2571F
#define COL_GREEN 0x2E9E4F
#define COL_RED 0xD9363E
#define COL_HEADER 0x1C1917
#define COL_SET_EDIT 0xF5C9B0
#define COL_HEADER_TEXT 0xFFFFFF

#define SCREEN_W 240
#define SCREEN_H 320
#define MAX_ROWS 5

static const char *const PORT_LABELS[POWER_METRICS_MAX_PORTS] = {"A1", "C1", "C2", "C3", "C4", "P6"};

typedef enum {
    SCREEN_OVERVIEW = 0,
    SCREEN_DETAIL,
    SCREEN_TOTAL,
    SCREEN_SETTINGS,
    SCREEN_PROVISIONING,
    SCREEN_WIFI,
    SCREEN_KEYBOARD,
    SCREEN_NUMERIC,
} screen_t;

static screen_t s_screen;
static int s_selected;
static bool s_pd_selftest;
static int s_detail_port;
static bool s_confirm_reset;
static app_config_t s_cfg;
static bool s_have_cfg;
static uint32_t s_chart_seq;
static int s_chart_points = 60;
static int64_t s_pd_req_ms;
static uint32_t s_ov_seq_seen = UINT32_MAX;
static int s_ov_sel_seen = -1;
static volatile int s_battery_percent = -1;
static bool s_lang_zh = true;
#define L(zh, en) (s_lang_zh ? (zh) : (en))
static lv_font_t s_font14;
static lv_font_t s_font20;
static uint8_t s_brightness = 100;

// 息屏：背光关掉、暂停界面刷新；任意功能键唤醒（硬件电源键不接 MCU）。
#define SCREEN_IDLE_OFF_MS (5 * 60 * 1000)
// 唤醒后丢弃按键事件的时间窗：双击判定会把单击事件推迟到松手之后，
// 只按 RELEASE 清标志会让“迟到”的单击再执行一次导航。
#define WAKE_IGNORE_MS 600
static bool s_screen_off;
static int64_t s_wake_ignore_until_ms;
static lv_timer_t *s_repeat_timer;
static int s_repeat_dir;
static bool s_suppress_click;
static void repeat_stop(void);
static int64_t s_last_activity_ms;

// 电量为 I2C 读取，放在独立任务里，避免任何阻塞落进 LVGL 任务。
static void battery_task(void *arg) {
    (void)arg;
    for (;;) {
        s_battery_percent = bsp_battery_soc();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static lv_obj_t *s_scr_overview;
static lv_obj_t *s_scr_detail;
static lv_obj_t *s_scr_total;
static lv_obj_t *s_tt_w;
static lv_obj_t *s_tt_usage;
static lv_obj_t *s_tt_bar;
static lv_obj_t *s_tt_name[MAX_ROWS];
static lv_obj_t *s_tt_state[MAX_ROWS];
static lv_obj_t *s_tt_chart;
static lv_chart_series_t *s_tt_series;
static lv_obj_t *s_tt_confirm;
static lv_obj_t *s_tt_confirm_title;
static lv_obj_t *s_tt_hint;
static uint16_t s_tt_state_hash;
static bool s_toggle_on;
static bool s_confirm_all;
static uint32_t s_tt_seq = UINT32_MAX;

static lv_obj_t *s_scr_settings;
static lv_obj_t *s_scr_provision;
static lv_obj_t *s_scr_wifi;
static lv_obj_t *s_scr_kbd;
static lv_obj_t *s_confirm_box;

static lv_obj_t *s_ov_total;
static lv_obj_t *s_ov_total_card;
static lv_obj_t *s_ov_cap;
static lv_obj_t *s_ov_usage;
static lv_obj_t *s_ov_usage_bg;
static lv_obj_t *s_ov_usage_bar;
static lv_obj_t *s_ov_battery;
static lv_obj_t *s_ov_hint;
static lv_obj_t *s_ov_row[MAX_ROWS];
static lv_obj_t *s_ov_bar[MAX_ROWS];
static lv_obj_t *s_ov_name[MAX_ROWS];
static lv_obj_t *s_ov_va[MAX_ROWS];
static lv_obj_t *s_ov_w[MAX_ROWS];

static lv_obj_t *s_dt_port;
static lv_obj_t *s_dt_w;
static lv_obj_t *s_dt_va;
static lv_obj_t *s_dt_proto;
static lv_obj_t *s_dt_out;
static lv_obj_t *s_dt_chart_title;
static lv_obj_t *s_dt_info[6];
static lv_obj_t *s_dt_chart;
static lv_chart_series_t *s_dt_series;
static lv_obj_t *s_out_confirm;
static lv_obj_t *s_out_confirm_title;
static bool s_confirm_output;

static int s_set_cursor;
static int s_set_top;
static bool s_set_edit;
static uint16_t s_ui_poll_ms = 1000;
static uint32_t s_screen_off_ms = 300000;
static bool s_ui_idle_pause = true;
static lv_obj_t *s_set_row[7];
static lv_obj_t *s_set_name[7];
static lv_obj_t *s_set_value[7];
static lv_obj_t *s_set_page;
static lv_obj_t *s_set_fw;
static lv_obj_t *s_set_footer;
static lv_obj_t *s_set_footer2;
static lv_obj_t *s_popup;
static lv_obj_t *s_popup_title;
static lv_obj_t *s_popup_opt[6];
static lv_obj_t *s_popup_hint;
static bool s_popup_open;
static int s_popup_item;
static int s_popup_cursor;
static int s_popup_count;

typedef enum { SET_INFO = 0, SET_CHOICE, SET_ACTION } set_kind_t;
typedef struct {
    const char *zh;
    const char *en;
    set_kind_t kind;
} set_item_t;

enum {
    SET_ID_WIFI = 0,
    SET_ID_IP,
    SET_ID_SIGNAL,
    SET_ID_RATE,
    SET_ID_IDLE,
    SET_ID_OFFTIME,
    SET_ID_BRIGHT,
    SET_ID_LANG,
    SET_ID_RESET,
};

static const set_item_t SET_ITEMS[] = {
    {"选择无线网络", "SELECT WI-FI", SET_ACTION},
    {"本机IP", "DEVICE IP", SET_INFO},
    {"信号", "SIGNAL", SET_INFO},
    {"轮询速度", "POLL SPEED", SET_CHOICE},
    {"息屏轮询", "POLL OFF", SET_CHOICE},
    {"息屏时间", "SCREEN OFF", SET_CHOICE},
    {"亮度", "BRIGHT", SET_CHOICE},
    {"语言", "LANGUAGE", SET_CHOICE},
    {"重置系统", "RESET", SET_ACTION},
};
#define SET_ITEM_COUNT ((int)(sizeof(SET_ITEMS) / sizeof(SET_ITEMS[0])))
#define SET_VISIBLE 7
static lv_obj_t *s_pv_ssid;

// LVGL 任务栈很小，快照接近 2KB：放在静态区，只在本文件的持锁路径里读写。
static power_link_view_t s_view;

static const power_link_view_t *view_refresh(void) {
    power_link_get(&s_view);
    return &s_view;
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color,
                            int x, int y, int w) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (w > 0) {
        lv_obj_set_width(label, w);
        lv_obj_set_pos(label, x, y);
    } else {
        lv_obj_set_pos(label, x, y);
    }
    return label;
}

static lv_obj_t *make_label_right(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color,
                                  int right_edge, int y, int w) {
    lv_obj_t *label = make_label(parent, text, font, color, right_edge - w, y, w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    return label;
}

static lv_obj_t *make_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static void make_hairline(lv_obj_t *parent, int x, int y, int w) {
    (void)make_box(parent, x, y, w, 1, COL_LINE, 0);
}

// ---------------------------------------------------------------- 总览

static void build_overview(void) {
    s_scr_overview = make_screen();

    // 顶部深色品牌条：CoCan 白色、Mirror 橙色（对齐小程序标识）
    make_box(s_scr_overview, 0, 0, 240, 30, COL_HEADER, 0);
    make_label(s_scr_overview, "CoCan", &s_font14, COL_HEADER_TEXT, 12, 7, 0);
    make_label(s_scr_overview, "Mirror", &s_font14, COL_AMBER, 58, 7, 0);
    s_ov_battery = make_label_right(s_scr_overview, "", &s_font14, COL_HEADER_TEXT,
                                    228, 7, 80);

    lv_obj_t *card = make_box(s_scr_overview, 10, 34, 220, 86, COL_CARD, 16);
    s_ov_total_card = card;
    make_label(card, L("总输出", "TOTAL OUTPUT"), &s_font14, COL_MUTED, 14, 6, 0);
    s_ov_total = make_label(card, "--", &lv_font_montserrat_44, COL_AMBER, 12, 26, 140);
    lv_label_set_long_mode(s_ov_total, LV_LABEL_LONG_CLIP);

    // 右侧：160W 上限 + 已用百分比 + 进度条（对齐小程序）
    s_ov_cap = make_label_right(card, "160W", &s_font14, COL_MUTED, 206, 4, 56);
    s_ov_usage = make_label_right(card, "0%", &s_font20, COL_AMBER, 206, 22, 56);
    lv_label_set_long_mode(s_ov_usage, LV_LABEL_LONG_CLIP);
    s_ov_usage_bg = make_box(card, 150, 56, 56, 6, COL_LINE, 3);
    s_ov_usage_bar = make_box(card, 150, 56, 3, 6, COL_AMBER, 3);

    for (int i = 0; i < MAX_ROWS; i++) {
        const int y = 122 + i * 35;
        s_ov_row[i] = make_box(s_scr_overview, 10, y, 220, 33, COL_CARD, 8);
        s_ov_bar[i] = make_box(s_ov_row[i], 6, 8, 3, 17, COL_CARD, 2);
        s_ov_name[i] = make_label(s_ov_row[i], PORT_LABELS[i], &s_font14, COL_TEXT, 14, 8, 24);
        s_ov_va[i] = make_label(s_ov_row[i], "--", &s_font14, COL_MUTED, 40, 8, 94);
        s_ov_w[i] = make_label_right(s_ov_row[i], "--", &s_font20, COL_TEXT, 208, 5, 76);
        lv_label_set_long_mode(s_ov_name[i], LV_LABEL_LONG_CLIP);
        lv_label_set_long_mode(s_ov_va[i], LV_LABEL_LONG_CLIP);
        lv_label_set_long_mode(s_ov_w[i], LV_LABEL_LONG_CLIP);
    }

    s_ov_hint = make_label(s_scr_overview, "", &s_font14, COL_RED, 10, 150, 220);
    lv_obj_set_height(s_ov_hint, 46);
    lv_obj_set_style_bg_opa(s_ov_hint, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ov_hint, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(s_ov_hint, 10, 0);
    lv_obj_set_style_text_align(s_ov_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s_ov_hint, 6, 0);
    lv_obj_add_flag(s_ov_hint, LV_OBJ_FLAG_HIDDEN);

    make_label(s_scr_overview, L("OK 详情", "OK DETAIL"), &s_font14, COL_DIM, 12, 302, 0);
    make_label_right(s_scr_overview, L("长按 OK 设置", "HOLD OK SETUP"), &s_font14, COL_DIM, 228, 302, 120);
}

// ---------------------------------------------------------------- 详情

static void build_detail(void) {
    s_scr_detail = make_screen();

    s_dt_port = make_label(s_scr_detail, L("接口 C1", "PORT C1"), &s_font14, COL_CYAN, 12, 10, 84);
    s_dt_out = make_label_right(s_scr_detail, L("输出: ?", "OUT: ?"), &s_font14, COL_DIM, 228, 10,
                                138);
    lv_label_set_long_mode(s_dt_port, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(s_dt_out, LV_LABEL_LONG_CLIP);

    lv_obj_t *card = make_box(s_scr_detail, 10, 34, 220, 112, COL_CARD, 16);
    s_dt_w = make_label(card, "--", &lv_font_montserrat_36, COL_AMBER, 0, 6, 220);
    lv_obj_set_style_text_align(s_dt_w, LV_TEXT_ALIGN_CENTER, 0);
    s_dt_va = make_label(card, "", &s_font14, COL_MUTED, 0, 54, 220);
    lv_obj_set_style_text_align(s_dt_va, LV_TEXT_ALIGN_CENTER, 0);
    s_dt_proto = make_label(card, "", &s_font14, COL_TEXT, 0, 80, 220);
    lv_obj_set_style_text_align(s_dt_proto, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *chart_card = make_box(s_scr_detail, 10, 222, 220, 70, COL_CARD, 14);
    s_dt_chart_title = make_label(chart_card, "", &s_font14, COL_MUTED, 14, 4, 120);
    lv_label_set_long_mode(s_dt_chart_title, LV_LABEL_LONG_CLIP);

    s_dt_chart = lv_chart_create(chart_card);
    lv_obj_set_pos(s_dt_chart, 8, 20);
    lv_obj_set_size(s_dt_chart, 204, 44);
    lv_obj_remove_flag(s_dt_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_dt_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dt_chart, 0, 0);
    lv_obj_set_style_pad_all(s_dt_chart, 0, 0);
    lv_chart_set_type(s_dt_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_dt_chart, POWER_HISTORY_LEN);
    lv_chart_set_axis_range(s_dt_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1800);
    lv_chart_set_div_line_count(s_dt_chart, 3, 4);
    lv_obj_set_style_line_color(s_dt_chart, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_dt_chart, LV_OPA_50, LV_PART_MAIN);
    s_dt_series = lv_chart_add_series(s_dt_chart, lv_color_hex(COL_AMBER), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_width(s_dt_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_dt_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_dt_chart, 2, LV_PART_ITEMS);

    // PD / 线材信息（云端 MCP 按需拉取），两列三行
    lv_obj_t *info_card = make_box(s_scr_detail, 10, 152, 220, 64, COL_CARD, 14);
    for (int row = 0; row < 3; row++) {
        s_dt_info[row * 2] = make_label(info_card, "", &s_font14, COL_TEXT, 10,
                                        4 + row * 20, 112);
        s_dt_info[row * 2 + 1] = make_label(info_card, "", &s_font14, COL_MUTED, 122,
                                            4 + row * 20, 88);
        lv_label_set_long_mode(s_dt_info[row * 2], LV_LABEL_LONG_CLIP);
        lv_label_set_long_mode(s_dt_info[row * 2 + 1], LV_LABEL_LONG_CLIP);
    }

    make_label(s_scr_detail, L("OK返回", "OK BACK"), &s_font14, COL_DIM, 12, 296, 64);
    lv_obj_t *dt_hint_out = make_label_right(s_scr_detail, L("长按OK开关输出", "HOLD OK: OUT"),
                                             &s_font14, COL_DIM, 238, 296, 120);
    lv_label_set_long_mode(dt_hint_out, LV_LABEL_LONG_CLIP);

    s_out_confirm = make_box(s_scr_detail, 20, 104, 200, 104, 0xFFF1EA, 16);
    lv_obj_add_flag(s_out_confirm, LV_OBJ_FLAG_HIDDEN);
    s_out_confirm_title = make_label(s_out_confirm, L("关闭 C1？", "TURN OFF C1?"), &s_font20, COL_RED, 0,
                                     16, 200);
    lv_obj_set_style_text_align(s_out_confirm_title, LV_TEXT_ALIGN_CENTER, 0);
    make_label(s_out_confirm, L("OK 确认", "OK = confirm"), &s_font14, COL_TEXT, 0, 56, 200);
    lv_obj_set_style_text_align(lv_obj_get_child(s_out_confirm, 1), LV_TEXT_ALIGN_CENTER, 0);
    make_label(s_out_confirm, L("短按任意键取消", "any click = cancel"), &s_font14, COL_MUTED, 0, 78, 200);
    lv_obj_set_style_text_align(lv_obj_get_child(s_out_confirm, 2), LV_TEXT_ALIGN_CENTER, 0);
}

// ---------------------------------------------------------------- 设置

static void load_screen(screen_t screen);
static void wifi_picker_open(void);
static void numeric_open(void);


// ---------------------------------------------------------------- 总输出详情

static void build_total(void) {
    s_scr_total = make_screen();

    lv_obj_t *tt_title = make_label(s_scr_total, L("总输出", "TOTAL OUTPUT"), &s_font14, COL_CYAN,
                                    0, 10, 240);
    lv_obj_set_style_text_align(tt_title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *card = make_box(s_scr_total, 10, 34, 220, 86, COL_CARD, 16);
    s_tt_w = make_label(card, "--", &lv_font_montserrat_44, COL_AMBER, 12, 26, 140);
    lv_label_set_long_mode(s_tt_w, LV_LABEL_LONG_CLIP);
    make_label_right(card, "160W", &s_font14, COL_MUTED, 206, 4, 56);
    s_tt_usage = make_label_right(card, "0%", &s_font20, COL_AMBER, 206, 22, 56);
    lv_label_set_long_mode(s_tt_usage, LV_LABEL_LONG_CLIP);
    make_box(card, 150, 56, 56, 6, COL_LINE, 3);
    s_tt_bar = make_box(card, 150, 56, 3, 6, COL_AMBER, 3);

    lv_obj_t *status_card = make_box(s_scr_total, 10, 126, 220, 108, COL_CARD, 14);
    make_label(status_card, L("接口状态", "PORT STATUS"), &s_font14, COL_MUTED, 12, 4, 140);
    for (int i = 0; i < MAX_ROWS; i++) {
        const int y = 24 + i * 16;
        s_tt_name[i] = make_label(status_card, PORT_LABELS[i], &s_font14, COL_TEXT, 12, y, 32);
        s_tt_state[i] = make_label_right(status_card, "--", &s_font14, COL_DIM, 208, y, 150);
        lv_label_set_long_mode(s_tt_state[i], LV_LABEL_LONG_CLIP);
    }

    lv_obj_t *chart_card = make_box(s_scr_total, 10, 240, 220, 58, COL_CARD, 12);
    make_label(chart_card, L("最近 60 秒", "LAST 60 s"), &s_font14, COL_MUTED, 12, 2, 120);
    s_tt_chart = lv_chart_create(chart_card);
    lv_obj_set_pos(s_tt_chart, 8, 18);
    lv_obj_set_size(s_tt_chart, 204, 34);
    lv_obj_set_style_bg_opa(s_tt_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tt_chart, 0, 0);
    lv_obj_set_style_pad_all(s_tt_chart, 0, 0);
    lv_chart_set_type(s_tt_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_tt_chart, 60);
    lv_chart_set_axis_range(s_tt_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1800);
    lv_chart_set_div_line_count(s_tt_chart, 2, 4);
    s_tt_series = lv_chart_add_series(s_tt_chart, lv_color_hex(COL_AMBER), LV_CHART_AXIS_PRIMARY_Y);
    // 与单个接口详情页一致：隐藏采样点，只画连线。
    lv_obj_set_style_width(s_tt_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_tt_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_tt_chart, 2, LV_PART_ITEMS);

    make_label(s_scr_total, L("OK返回", "OK BACK"), &s_font14, COL_DIM, 12, 302, 84);
    s_tt_hint = make_label_right(s_scr_total, "", &s_font14, COL_DIM, 236, 302, 140);
    lv_label_set_long_mode(s_tt_hint, LV_LABEL_LONG_CLIP);

    s_tt_confirm = make_box(s_scr_total, 20, 100, 200, 96, COL_CARD, 14);
    lv_obj_set_style_border_width(s_tt_confirm, 1, 0);
    lv_obj_set_style_border_color(s_tt_confirm, lv_color_hex(COL_LINE), 0);
    s_tt_confirm_title = make_label(s_tt_confirm, "", &s_font14, COL_RED, 0, 16, 200);
    lv_obj_set_style_text_align(s_tt_confirm_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *hint = make_label(s_tt_confirm, L("OK 确认   其它键取消", "OK = yes   any = cancel"),
                                &s_font14, COL_MUTED, 0, 52, 200);
    lv_obj_set_style_text_align(lv_obj_get_child(s_tt_confirm, 0), LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_tt_confirm, LV_OBJ_FLAG_HIDDEN);
}


static int s_tt_points(void) {
    return 30;
}

static void update_total(const power_link_view_t *view) {
    uint32_t total_mw = 0;
    for (int i = 0; i < POWER_METRICS_MAX_PORTS; i++) {
        total_mw += power_port_milliwatts(&view->metrics.ports[i]);
    }

    char buf[24];
    if (view->poll_ok == 0 && view->state != POWER_LINK_LIVE) {
        lv_obj_set_style_text_font(s_tt_w, &lv_font_montserrat_44, 0);
        lv_label_set_text(s_tt_w, "--");
        lv_label_set_text(s_tt_usage, "--");
        lv_obj_set_width(s_tt_bar, 3);
    } else {
        power_format_watts(total_mw, buf, sizeof(buf));
        char text[32];
        snprintf(text, sizeof(text), "%sW", buf);
        const lv_font_t *font =
            strlen(text) >= 5 ? &lv_font_montserrat_36 : &lv_font_montserrat_44;
        lv_obj_set_style_text_font(s_tt_w, font, 0);
        lv_label_set_text(s_tt_w, text);

        uint32_t pct10 = total_mw / 160u;
        if (pct10 > 1000) pct10 = 1000;
        char usage[16];
        if (pct10 >= 100) snprintf(usage, sizeof(usage), "%u%%", (unsigned)(pct10 / 10));
        else snprintf(usage, sizeof(usage), "%u.%u%%", (unsigned)(pct10 / 10), (unsigned)(pct10 % 10));
        lv_label_set_text(s_tt_usage, usage);
        int bar_w = (int)(pct10 * 56 / 1000);
        if (bar_w < 3) bar_w = 3;
        lv_obj_set_width(s_tt_bar, bar_w);
    }

    uint16_t state_hash = 0;
    for (int i = 0; i < MAX_ROWS; i++) {
        state_hash = (uint16_t)(state_hash * 31 + (view->port_on_valid[i] ? 1 : 2) +
                                (view->port_on[i] ? 3 : 5));
    }
    const bool dirty = (view->seq != s_tt_seq) || (state_hash != s_tt_state_hash);
    if (state_hash != s_tt_state_hash) {
        s_tt_state_hash = state_hash;
        bool any_off = false;
        for (int i = 0; i < MAX_ROWS; i++) {
            if (view->port_on_valid[i] && !view->port_on[i]) any_off = true;
        }
        lv_label_set_text(s_tt_hint, any_off ? L("长按OK 开启所有接口", "HOLD OK: ALL ON")
                                             : L("长按OK 关闭所有接口", "HOLD OK: ALL OFF"));
    }
    if (dirty) {
        for (int i = 0; i < MAX_ROWS; i++) {
            const bool exists = i < view->metrics.port_count;
            const power_port_metrics_t *port = &view->metrics.ports[i];
            const bool active = exists && power_port_active(port);
            const bool out_off = view->port_on_valid[i] && !view->port_on[i];
            if (!exists) {
                lv_label_set_text(s_tt_state[i], "--");
                lv_obj_set_style_text_color(s_tt_state[i], lv_color_hex(COL_DIM), 0);
            } else if (out_off) {
                lv_label_set_text(s_tt_state[i], "OFF");
                lv_obj_set_style_text_color(s_tt_state[i], lv_color_hex(COL_RED), 0);
            } else if (!active) {
                lv_label_set_text(s_tt_state[i], L("待机", "STANDBY"));
                lv_obj_set_style_text_color(s_tt_state[i], lv_color_hex(COL_DIM), 0);
            } else {
                char wbuf[16];
                power_format_watts(power_port_milliwatts(port), wbuf, sizeof(wbuf));
                char line[32];
                snprintf(line, sizeof(line), L("输出中 %sW", "OUT %sW"), wbuf);
                lv_label_set_text(s_tt_state[i], line);
                lv_obj_set_style_text_color(s_tt_state[i], lv_color_hex(COL_AMBER), 0);
            }
            lv_obj_set_style_text_color(s_tt_name[i], lv_color_hex(exists ? COL_TEXT : COL_DIM), 0);
        }

        const int n = s_tt_points();
        int32_t raw[POWER_HISTORY_LEN];
        int32_t values[POWER_HISTORY_LEN];
        const int len = view->history_len;
        for (int i = 0; i < n; i++) {
            const int src = len - n + i;
            raw[i] = src >= 0 ? (int32_t)(view->total_history[(view->history_head - len + src + POWER_HISTORY_LEN) % POWER_HISTORY_LEN] / 100) : 0;
            values[i] = 0;
        }
        for (int i = 0; i < n; i++) {
            int32_t sum = 0;
            int cnt = 0;
            for (int k = -2; k <= 2; k++) {
                const int j = i + k;
                if (j < 0 || j >= n) continue;
                if (len - n + j < 0) continue;
                sum += raw[j];
                cnt++;
            }
            values[i] = cnt > 0 ? sum / cnt : raw[i];
        }

        int32_t vmin = INT32_MAX;
        int32_t vmax = INT32_MIN;
        for (int i = 0; i < n; i++) {
            if (len - n + i < 0) continue;
            if (values[i] < vmin) vmin = values[i];
            if (values[i] > vmax) vmax = values[i];
        }
        int32_t lo = 0, hi = 100;
        if (vmax >= vmin) {
            const int32_t span = vmax - vmin;
            if (span < 20) {
                const int32_t mid = (vmax + vmin) / 2;
                lo = mid - 10;
                hi = mid + 10;
            } else {
                const int32_t pad = span / 4 + 5;
                lo = vmin - pad;
                hi = vmax + pad;
            }
            if (lo < 0) lo = 0;
            if (hi - lo < 10) hi = lo + 10;
        }
        if (lv_chart_get_point_count(s_tt_chart) != (uint32_t)n) {
            lv_chart_set_point_count(s_tt_chart, (uint32_t)n);
        }
        lv_chart_set_axis_range(s_tt_chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
        lv_chart_set_series_values(s_tt_chart, s_tt_series, values, (uint32_t)n);
        s_tt_seq = view->seq;
    }
}

static void open_total(void) {
    s_confirm_all = false;
    lv_obj_add_flag(s_tt_confirm, LV_OBJ_FLAG_HIDDEN);
    s_tt_seq = UINT32_MAX;
    load_screen(SCREEN_TOTAL);
    update_total(view_refresh());
}

static void build_settings(void) {
    s_scr_settings = make_screen();
    make_label(s_scr_settings, L("设置", "SETUP"), &s_font14, COL_CYAN, 12, 10, 80);
    s_set_page = make_label_right(s_scr_settings, "1/11", &s_font14, COL_DIM, 228, 10, 70);

    for (int i = 0; i < SET_VISIBLE; i++) {
        const int y = 34 + i * 30;
        s_set_row[i] = make_box(s_scr_settings, 10, y, 220, 28, COL_BG, 8);
        s_set_name[i] = make_label(s_set_row[i], "", &s_font14, COL_MUTED, 4, 5, 104);
        s_set_value[i] = make_label_right(s_set_row[i], "", &s_font14, COL_TEXT, 212, 5, 104);
        make_box(s_scr_settings, 16, y + 29, 204, 1, COL_LINE, 0);
        lv_label_set_long_mode(s_set_name[i], LV_LABEL_LONG_CLIP);
        lv_label_set_long_mode(s_set_value[i], LV_LABEL_LONG_CLIP);
    }

    char fw_text[32];
    snprintf(fw_text, sizeof(fw_text), L("固件 %s", "FW %s"), esp_app_get_description()->version);
    s_set_fw = make_label(s_scr_settings, fw_text, &s_font14, COL_DIM, 0, 282, 240);
    lv_obj_set_style_text_align(s_set_fw, LV_TEXT_ALIGN_CENTER, 0);

    s_set_footer = make_label(s_scr_settings, L("OK 选择", "OK SELECT"),
                              &s_font14, COL_DIM, 12, 256, 100);
    lv_label_set_long_mode(s_set_footer, LV_LABEL_LONG_CLIP);
    s_set_footer2 = make_label_right(s_scr_settings, L("长按OK 返回", "HOLD OK BACK"),
                                     &s_font14, COL_DIM, 228, 256, 110);
    lv_label_set_long_mode(s_set_footer2, LV_LABEL_LONG_CLIP);

    s_popup = make_box(s_scr_settings, 18, 72, 204, 176, 0xFFF1EA, 14);
    lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    s_popup_title = make_label(s_popup, "", &s_font20, COL_RED, 0, 10, 204);
    lv_obj_set_style_text_align(s_popup_title, LV_TEXT_ALIGN_CENTER, 0);
    for (int i = 0; i < 6; i++) {
        s_popup_opt[i] = make_label(s_popup, "", &s_font14, COL_MUTED, 0, 40 + i * 24, 204);
        lv_obj_set_style_text_align(s_popup_opt[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    s_popup_hint = make_label(s_popup, "", &s_font14, COL_DIM, 0, 150, 204);
    lv_obj_set_style_text_align(s_popup_hint, LV_TEXT_ALIGN_CENTER, 0);

    s_confirm_box = make_box(s_scr_settings, 20, 96, 200, 120, 0xFFF1EA, 16);
    lv_obj_add_flag(s_confirm_box, LV_OBJ_FLAG_HIDDEN);
    make_label(s_confirm_box, L("重置系统？", "FACTORY RESET?"), &s_font20, COL_RED, 0, 18, 200);
    lv_obj_set_style_text_align(lv_obj_get_child(s_confirm_box, 0), LV_TEXT_ALIGN_CENTER, 0);
    make_label(s_confirm_box, L("清空全部配置并重新配网", "Erase all, then re-pair"), &s_font14, COL_TEXT, 0, 58, 200);
    lv_obj_set_style_text_align(lv_obj_get_child(s_confirm_box, 1), LV_TEXT_ALIGN_CENTER, 0);
    make_label(s_confirm_box, L("短按任意键取消", "any click = cancel"), &s_font14, COL_MUTED, 0, 76, 200);
    lv_obj_set_style_text_align(lv_obj_get_child(s_confirm_box, 2), LV_TEXT_ALIGN_CENTER, 0);
    make_label(s_confirm_box, L("长按 OK 确认重置", "HOLD OK = reset"), &s_font14, COL_RED, 0, 98, 200);
    lv_obj_set_style_text_align(lv_obj_get_child(s_confirm_box, 3), LV_TEXT_ALIGN_CENTER, 0);
}


// ---------------------------------------------------------------- Wi-Fi 选择

#define WIFI_MAX_APS 6
#define KBD_MAX_LEN 64

static wifi_ap_record_t s_ap[WIFI_MAX_APS];
static volatile int s_ap_count;
static volatile bool s_scan_busy;
static int64_t s_scan_start_ms;
static int s_ap_cursor;
static char s_sel_ssid[33];
static char s_kbd_pwd[KBD_MAX_LEN + 1];
static int s_kbd_len;
static int s_kbd_cursor;
static bool s_kbd_upper;
static int s_kbd_page;  // 0 = 字母, 1 = 符号

static lv_obj_t *s_wifi_row[WIFI_MAX_APS];
static lv_obj_t *s_wifi_name[WIFI_MAX_APS];
static lv_obj_t *s_wifi_rssi[WIFI_MAX_APS];
static lv_obj_t *s_wifi_hint;


static TaskHandle_t s_scan_task;
static StaticTask_t s_scan_tcb;
static StackType_t s_scan_stack[3072 / sizeof(StackType_t)];

static void wifi_scan_task(void *arg) {
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        wifi_scan_config_t cfg = {0};
        cfg.scan_type = WIFI_SCAN_TYPE_PASSIVE;
        cfg.scan_time.passive = 300;

        power_link_set_scanning(true);
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_LOGI(TAG, "扫描任务开始");
        (void)esp_wifi_scan_stop();
        vTaskDelay(pdMS_TO_TICKS(200));

        esp_err_t err = esp_wifi_scan_start(&cfg, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "扫描启动失败(%s)，重试", esp_err_to_name(err));
            (void)esp_wifi_scan_stop();
            vTaskDelay(pdMS_TO_TICKS(500));
            err = esp_wifi_scan_start(&cfg, false);
        }
        ESP_LOGI(TAG, "扫描启动返回: %s", esp_err_to_name(err));

        uint16_t num = 0;
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) num = 0;
            if (num > WIFI_MAX_APS) num = WIFI_MAX_APS;
            if (num == 0 || esp_wifi_scan_get_ap_records(&num, s_ap) != ESP_OK) num = 0;
            (void)esp_wifi_scan_stop();
        }
        s_ap_count = num;
        ESP_LOGI(TAG, "扫描完成: %u 个网络 (err=%s)", (unsigned)num, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(200));
        power_link_set_scanning(false);
        s_scan_busy = false;
    }
}

static void wifi_scan_start(void) {
    if (s_scan_busy) return;
    s_scan_busy = true;
    s_ap_count = 0;
    s_ap_cursor = 0;
    s_scan_start_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "开始扫描 Wi-Fi");
    if (s_scan_task) {
        xTaskNotifyGive(s_scan_task);
    } else {
        ESP_LOGW(TAG, "扫描任务未就绪");
        s_scan_busy = false;
    }
}

// esp_wifi_ap_record_t.ssid 是 33 字节定长，最长 32 字符时没有结束符。
static void copy_ssid32(const uint8_t *src, char *dst, size_t dst_len) {
    size_t n = 0;
    while (n < 32 && src[n] != 0) n++;
    if (n > dst_len - 1) n = dst_len - 1;
    for (size_t i = 0; i < n; i++) dst[i] = (char)src[i];
    dst[n] = '\0';
}

// 把 SSID 清洗成合法 UTF-8；非法字节替换为 '?'，避免 LVGL 文本解析卡死。
static void display_ssid(const char *src, char *out, size_t len) {
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < len;) {
        const unsigned char c = (unsigned char)src[i];
        int n = 0;
        if (c < 0x80) n = 1;
        else if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;

        bool valid = (n > 0);
        for (int k = 1; valid && k < n; k++) {
            if (src[i + k] == '\0' || (src[i + k] & 0xC0) != 0x80) valid = false;
        }
        if (!valid) {
            out[o++] = '?';
            i++;
            continue;
        }
        for (int k = 0; k < n && o + 1 < len; k++) out[o++] = src[i + k];
        i += (size_t)n;
    }
    out[o] = '\0';
}

static void wifi_render(void) {
    if (!s_scr_wifi) return;
    if (s_scan_busy && s_scan_start_ms > 0 &&
        esp_timer_get_time() / 1000 - s_scan_start_ms > 10000) {
        ESP_LOGW(TAG, "扫描超时，复位状态");
        s_scan_busy = false;
        s_ap_count = 0;
    }
    for (int i = 0; i < WIFI_MAX_APS; i++) {
        if (s_scan_busy || i >= s_ap_count) {
            lv_obj_add_flag(s_wifi_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_wifi_row[i], LV_OBJ_FLAG_HIDDEN);
        char line[64];
        char raw[33];
        char ssid[48];
        copy_ssid32(s_ap[i].ssid, raw, sizeof(raw));
        display_ssid(raw, ssid, sizeof(ssid));
        snprintf(line, sizeof(line), "%s   %d dBm", ssid, s_ap[i].rssi);
        lv_label_set_text(s_wifi_row[i], line);
        const bool cur = (i == s_ap_cursor);
        lv_obj_set_style_bg_color(s_wifi_row[i], lv_color_hex(cur ? COL_CARD_SEL : COL_CARD), 0);
        lv_obj_set_style_text_color(s_wifi_row[i], lv_color_hex(cur ? COL_AMBER : COL_TEXT), 0);
    }
    if (s_scan_busy) lv_label_set_text(s_wifi_hint, L("扫描中…", "scanning..."));
    else if (s_ap_count == 0) lv_label_set_text(s_wifi_hint, L("未找到网络，按 OK 重扫", "no networks, OK to retry"));
    else lv_label_set_text(s_wifi_hint, L("OK 连接  长按OK 返回", "OK connect  HOLD OK back"));
}

static void build_wifi(void) {
    s_scr_wifi = make_screen();
    make_label(s_scr_wifi, L("选择 Wi-Fi", "SELECT WI-FI"), &s_font14, COL_CYAN, 12, 10, 120);

    for (int i = 0; i < WIFI_MAX_APS; i++) {
        const int y = 40 + i * 34;
        lv_obj_t *row = make_label(s_scr_wifi, "", &s_font14, COL_TEXT, 10, y, 220);
        lv_obj_set_height(row, 32);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_left(row, 10, 0);
        lv_label_set_long_mode(row, LV_LABEL_LONG_CLIP);
        s_wifi_row[i] = row;
    }

    s_wifi_hint = make_label(s_scr_wifi, "", &s_font14, COL_DIM, 12, 254, 216);
    lv_label_set_long_mode(s_wifi_hint, LV_LABEL_LONG_CLIP);

    lv_obj_t *warn = make_label(s_scr_wifi, L("请连接信任的 Wi-Fi", "Connect trusted Wi-Fi"),
                                &s_font14, COL_RED, 0, 292, 240);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);
}

// ---------------------------------------------------------------- 键盘

typedef enum { K_CHAR = 0, K_SHIFT, K_BACK, K_PAGE, K_SPACE, K_OK } kbd_kind_t;

typedef struct {
    const char *label;
    kbd_kind_t kind;
    char ch;
} kbd_key_t;

// 前 3 行 x 10 列
static const kbd_key_t KBD_LOWER[3][10] = {
    {{"Q", K_CHAR, 'q'}, {"W", K_CHAR, 'w'}, {"E", K_CHAR, 'e'}, {"R", K_CHAR, 'r'},
     {"T", K_CHAR, 't'}, {"Y", K_CHAR, 'y'}, {"U", K_CHAR, 'u'}, {"I", K_CHAR, 'i'},
     {"O", K_CHAR, 'o'}, {"P", K_CHAR, 'p'}},
    {{NULL, K_CHAR, 0}, {"A", K_CHAR, 'a'}, {"S", K_CHAR, 's'}, {"D", K_CHAR, 'd'},
     {"F", K_CHAR, 'f'}, {"G", K_CHAR, 'g'}, {"H", K_CHAR, 'h'}, {"J", K_CHAR, 'j'},
     {"K", K_CHAR, 'k'}, {"L", K_CHAR, 'l'}},
    {{"^", K_SHIFT, 0}, {"Z", K_CHAR, 'z'}, {"X", K_CHAR, 'x'}, {"C", K_CHAR, 'c'},
     {"V", K_CHAR, 'v'}, {"B", K_CHAR, 'b'}, {"N", K_CHAR, 'n'}, {"M", K_CHAR, 'm'},
     {"<", K_BACK, 0}, {NULL, K_CHAR, 0}},
};

static const kbd_key_t KBD_SYMBOL[3][10] = {
    {{"1", K_CHAR, '1'}, {"2", K_CHAR, '2'}, {"3", K_CHAR, '3'}, {"4", K_CHAR, '4'},
     {"5", K_CHAR, '5'}, {"6", K_CHAR, '6'}, {"7", K_CHAR, '7'}, {"8", K_CHAR, '8'},
     {"9", K_CHAR, '9'}, {"0", K_CHAR, '0'}},
    {{"-", K_CHAR, '-'}, {"/", K_CHAR, '/'}, {":", K_CHAR, ':'}, {";", K_CHAR, ';'},
     {"(", K_CHAR, '('}, {")", K_CHAR, ')'}, {"$", K_CHAR, '$'}, {"&", K_CHAR, '&'},
     {"@", K_CHAR, '@'}, {"\"", K_CHAR, '"'}},
    {{"+", K_CHAR, '+'}, {"=", K_CHAR, '='}, {".", K_CHAR, '.'}, {",", K_CHAR, ','},
     {"?", K_CHAR, '?'}, {"!", K_CHAR, '!'}, {"'", K_CHAR, '\''}, {"<", K_BACK, 0},
     {NULL, K_CHAR, 0}, {NULL, K_CHAR, 0}},
};

// 底部一行：切换页 / 空格 / 确定
static const kbd_key_t KBD_BOTTOM[2][3] = {
    {{"123", K_PAGE, 0}, {"SPACE", K_SPACE, 0}, {"OK", K_OK, 0}},
    {{"abc", K_PAGE, 0}, {"SPACE", K_SPACE, 0}, {"OK", K_OK, 0}},
};

#define KBD_GRID_KEYS 30
#define KBD_TOTAL_KEYS (KBD_GRID_KEYS + 3)

static lv_obj_t *s_kbd_slot[3][10];
static lv_obj_t *s_kbd_bottom[3];
static lv_obj_t *s_kbd_field;
static lv_obj_t *s_kbd_ssid;

static const kbd_key_t *kbd_key_for(int index) {
    if (index >= KBD_GRID_KEYS && index < KBD_TOTAL_KEYS) {
        return &KBD_BOTTOM[s_kbd_page][index - KBD_GRID_KEYS];
    }
    if (index >= KBD_GRID_KEYS) return NULL;
    const int r = index / 10;
    const int c = index % 10;
    const kbd_key_t *key = s_kbd_page == 0 ? &KBD_LOWER[r][c] : &KBD_SYMBOL[r][c];
    return key->label ? key : NULL;
}

static void kbd_render(void) {
    if (!s_scr_kbd) return;

    lv_label_set_text(s_kbd_field, s_kbd_pwd);

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 10; c++) {
            const int index = r * 10 + c;
            const kbd_key_t *key = s_kbd_page == 0 ? &KBD_LOWER[r][c] : &KBD_SYMBOL[r][c];
            lv_obj_t *slot = s_kbd_slot[r][c];
            if (!key->label) {
                lv_obj_add_flag(slot, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_remove_flag(slot, LV_OBJ_FLAG_HIDDEN);

            char label[8];
            if (key->kind == K_CHAR) {
                label[0] = s_kbd_upper ? (char)(key->ch - 'a' + 'A') : key->ch;
                label[1] = '\0';
            } else {
                snprintf(label, sizeof(label), "%s", key->label);
            }
            lv_label_set_text(slot, label);

            const bool cur = (index == s_kbd_cursor);
            lv_obj_set_style_bg_color(slot, lv_color_hex(cur ? COL_AMBER : COL_CARD), 0);
            lv_obj_set_style_text_color(slot, lv_color_hex(cur ? COL_CARD : COL_TEXT), 0);
        }
    }

    for (int i = 0; i < 3; i++) {
        const kbd_key_t *key = &KBD_BOTTOM[s_kbd_page][i];
        lv_obj_t *slot = s_kbd_bottom[i];
        lv_label_set_text(slot, key->label);
        const bool cur = (s_kbd_cursor == KBD_GRID_KEYS + i);
        lv_obj_set_style_bg_color(slot, lv_color_hex(cur ? COL_AMBER : COL_CARD), 0);
        lv_obj_set_style_text_color(slot, lv_color_hex(cur ? COL_CARD : COL_TEXT), 0);
    }
}

static void build_keyboard(void) {
    s_scr_kbd = make_screen();
    s_kbd_ssid = make_label(s_scr_kbd, "", &s_font14, COL_MUTED, 12, 8, 216);
    lv_label_set_long_mode(s_kbd_ssid, LV_LABEL_LONG_CLIP);
    s_kbd_field = make_label(s_scr_kbd, "", &s_font14, COL_AMBER, 12, 30, 216);
    lv_label_set_long_mode(s_kbd_field, LV_LABEL_LONG_CLIP);

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 10; c++) {
            const int x = 4 + c * 23;
            const int y = 70 + r * 40;
            lv_obj_t *slot = make_label(s_scr_kbd, "", &s_font14, COL_TEXT, x, y, 21);
            lv_obj_set_height(slot, 34);
            lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(slot, lv_color_hex(COL_CARD), 0);
            lv_obj_set_style_radius(slot, 5, 0);
            lv_obj_set_style_text_align(slot, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(slot, LV_LABEL_LONG_CLIP);
            s_kbd_slot[r][c] = slot;
        }
    }

    const int by = 70 + 3 * 40;
    const int bx[3] = {4, 56, 186};
    const int bw[3] = {46, 126, 50};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *slot = make_label(s_scr_kbd, "", &s_font14, COL_TEXT, bx[i], by, bw[i]);
        lv_obj_set_height(slot, 34);
        lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(slot, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_radius(slot, 5, 0);
        lv_obj_set_style_text_align(slot, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(slot, LV_LABEL_LONG_CLIP);
        s_kbd_bottom[i] = slot;
    }

    make_label(s_scr_kbd, L("双击OK 退格  长按OK 返回", "DBL OK: DEL  HOLD: BACK"), &s_font14, COL_DIM, 12, 290, 216);
}


// ---------------------------------------------------------------- 数字键盘(CP-IP)

static lv_obj_t *s_scr_num;
static lv_obj_t *s_num_field;
static lv_obj_t *s_num_key[12];
static lv_obj_t *s_num_ok;
static char s_num_buf[20];
static int s_num_cursor;

static const char *const NUM_KEYS[12] = {"1", "2", "3", "4", "5", "6",
                                         "7", "8", "9", ".", "0", "<"};

static void numeric_render(void) {
    if (!s_scr_num) return;
    lv_label_set_text(s_num_field, s_num_buf);
    for (int i = 0; i < 12; i++) {
        const bool cur = (i == s_num_cursor);
        lv_obj_set_style_bg_color(s_num_key[i], lv_color_hex(cur ? COL_AMBER : COL_CARD), 0);
        lv_obj_set_style_text_color(s_num_key[i], lv_color_hex(cur ? COL_CARD : COL_TEXT), 0);
    }
    if (s_num_ok) {
        const bool cur = (s_num_cursor == 12);
        lv_obj_set_style_bg_color(s_num_ok, lv_color_hex(cur ? COL_AMBER : COL_CARD), 0);
        lv_obj_set_style_text_color(s_num_ok, lv_color_hex(cur ? COL_CARD : COL_TEXT), 0);
    }
}

static void build_numeric(void) {
    s_scr_num = make_screen();
    make_label(s_scr_num, L("充电器 IP", "CHARGER IP"), &s_font14, COL_CYAN, 12, 10, 120);
    lv_obj_t *num_hint = make_label(s_scr_num, L("双击OK 退格  长按OK 返回", "DBL OK: DEL  HOLD: BACK"), &s_font14, COL_DIM, 12, 300, 216);
    lv_label_set_long_mode(num_hint, LV_LABEL_LONG_CLIP);
    s_num_field = make_label(s_scr_num, "", &s_font20, COL_AMBER, 12, 38, 216);
    lv_label_set_long_mode(s_num_field, LV_LABEL_LONG_CLIP);

    for (int i = 0; i < 12; i++) {
        const int c = i % 3;
        const int r = i / 3;
        const int x = 20 + c * 70;
        const int y = 78 + r * 46;
        lv_obj_t *key = make_label(s_scr_num, NUM_KEYS[i], &s_font20, COL_TEXT, x, y, 64);
        lv_obj_set_height(key, 40);
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(key, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_radius(key, 8, 0);
        lv_obj_set_style_text_align(key, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(key, LV_LABEL_LONG_CLIP);
        s_num_key[i] = key;
    }

    lv_obj_t *ok = make_label(s_scr_num, L("确定", "OK"), &s_font20, COL_CARD, 20, 262, 214);
    lv_obj_set_height(ok, 36);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_radius(ok, 8, 0);
    lv_obj_set_style_text_align(ok, LV_TEXT_ALIGN_CENTER, 0);
    s_num_ok = ok;
}

static void numeric_open(void) {
    snprintf(s_num_buf, sizeof(s_num_buf), "%.19s", s_have_cfg ? s_cfg.host : "");
    s_num_cursor = 0;
    load_screen(SCREEN_NUMERIC);
    numeric_render();
}

static void numeric_apply(void) {
    char host[APP_CONFIG_HOST_MAX];
    app_config_normalize_host(s_num_buf, host, sizeof(host));
    if (host[0] == '\0' || !s_have_cfg) return;
    app_config_t tmp = s_cfg;
    snprintf(tmp.host, sizeof(tmp.host), "%s", host);
    if (app_config_save(&tmp) == ESP_OK) esp_restart();
}

static void numeric_backspace(void) {
    size_t n = strlen(s_num_buf);
    if (n > 0) {
        s_num_buf[n - 1] = 0;
        numeric_render();
    }
}

static void numeric_press(int index) {
    if (index == 12) {
        numeric_apply();
        return;
    }
    const size_t len = strlen(s_num_buf);
    if (index == 11) {  // 退格
        if (len > 0) s_num_buf[len - 1] = '\0';
        return;
    }
    if (len + 1 < sizeof(s_num_buf)) {
        s_num_buf[len] = NUM_KEYS[index][0];
        s_num_buf[len + 1] = '\0';
    }
}

static void numeric_move(int delta) {
    s_num_cursor += delta;
    if (s_num_cursor < 0) s_num_cursor = 0;
    if (s_num_cursor > 12) s_num_cursor = 12;
    numeric_render();
}

static void wifi_picker_open(void) {
    s_ap_cursor = 0;
    wifi_scan_start();
    load_screen(SCREEN_WIFI);
    wifi_render();
}

static void wifi_pick_current(void) {
    if (s_scan_busy || s_ap_cursor < 0 || s_ap_cursor >= s_ap_count) return;
    copy_ssid32(s_ap[s_ap_cursor].ssid, s_sel_ssid, sizeof(s_sel_ssid));
    if (s_ap[s_ap_cursor].authmode == WIFI_AUTH_OPEN) {
        if (s_have_cfg) {
            snprintf(s_cfg.ssid, sizeof(s_cfg.ssid), "%s", s_sel_ssid);
            s_cfg.password[0] = '\0';
            (void)app_config_save(&s_cfg);
        }
        esp_restart();
        return;
    }
    char shown[48];
    display_ssid(s_sel_ssid, shown, sizeof(shown));
    ESP_LOGI(TAG, "选择 SSID: %s", shown);
    lv_label_set_text(s_kbd_ssid, shown);
    ESP_LOGI(TAG, "切换到键盘页");
    s_kbd_pwd[0] = '\0';
    s_kbd_len = 0;
    s_kbd_cursor = 0;
    s_kbd_page = 0;
    s_kbd_upper = false;
    load_screen(SCREEN_KEYBOARD);
    kbd_render();
}

static void kbd_confirm(void) {
    if (s_kbd_len == 0) return;
    if (s_have_cfg) {
        snprintf(s_cfg.ssid, sizeof(s_cfg.ssid), "%s", s_sel_ssid);
        snprintf(s_cfg.password, sizeof(s_cfg.password), "%s", s_kbd_pwd);
        (void)app_config_save(&s_cfg);
    }
    if (s_kbd_field) lv_label_set_text(s_kbd_field, L("保存中…", "saving..."));
    esp_restart();
}

static void kbd_backspace(void) {
    if (s_kbd_len > 0) {
        s_kbd_len--;
        s_kbd_pwd[s_kbd_len] = 0;
    }
    kbd_render();
}

static void kbd_apply_key(void) {
    const kbd_key_t *key = kbd_key_for(s_kbd_cursor);
    if (!key) return;

    switch (key->kind) {
    case K_CHAR:
        if (s_kbd_len < KBD_MAX_LEN) {
            char ch = key->ch;
            if (s_kbd_upper && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            s_kbd_pwd[s_kbd_len++] = ch;
            s_kbd_pwd[s_kbd_len] = '\0';
        }
        break;
    case K_SHIFT:
        s_kbd_upper = !s_kbd_upper;
        break;
    case K_BACK:
        if (s_kbd_len > 0) s_kbd_pwd[--s_kbd_len] = '\0';
        break;
    case K_PAGE:
        s_kbd_page = !s_kbd_page;
        s_kbd_cursor = 0;
        break;
    case K_SPACE:
        if (s_kbd_len < KBD_MAX_LEN) {
            s_kbd_pwd[s_kbd_len++] = ' ';
            s_kbd_pwd[s_kbd_len] = '\0';
        }
        break;
    case K_OK:
        kbd_confirm();
        break;
    }
    kbd_render();
}

static void kbd_move(int delta) {
    for (int step = 0; step < KBD_TOTAL_KEYS + 1; step++) {
        s_kbd_cursor += delta;
        if (s_kbd_cursor < 0) s_kbd_cursor = KBD_TOTAL_KEYS - 1;
        if (s_kbd_cursor >= KBD_TOTAL_KEYS) s_kbd_cursor = 0;
        if (kbd_key_for(s_kbd_cursor)) break;
    }
    kbd_render();
}

// ---------------------------------------------------------------- 配网

static void build_provisioning(void) {
    s_scr_provision = make_screen();

    make_label(s_scr_provision, L("配网", "SET UP"), &s_font14, COL_AMBER, 12, 10, 100);
    make_label(s_scr_provision, L("连接本设备\n到你的 Wi-Fi", "Connect this device\nto your Wi-Fi"), &s_font20, COL_TEXT,
               12, 34, 216);

    lv_obj_t *card = make_box(s_scr_provision, 10, 104, 220, 92, COL_CARD, 14);
    make_label(card, L("连接热点", "JOIN WI-FI"), &s_font14, COL_MUTED, 14, 10, 100);
    s_pv_ssid = make_label(card, "AIPassport-0000", &s_font20, COL_AMBER, 14, 32, 196);
    make_label(card, L("开放网络，无需密码", "open, no password"), &s_font14, COL_DIM, 14, 66, 196);

    lv_obj_t *url_card = make_box(s_scr_provision, 10, 206, 220, 56, COL_CARD, 14);
    make_label(url_card, L("然后打开", "THEN OPEN"), &s_font14, COL_MUTED, 14, 8, 100);
    make_label(url_card, "http://" PROVISIONING_AP_IP, &s_font20, COL_CYAN, 14, 28, 196);

    make_label(s_scr_provision, L("保存后设备重启\n进入监控界面", "Saved settings restart\nthe device into meter"), &s_font14,
               COL_DIM, 12, 270, 216);
}

// ---------------------------------------------------------------- 更新

static void update_overview(const power_link_view_t *view) {
    const power_discover_t ds = power_link_discover_state();
    if (s_ov_hint) {
        if (ds == POWER_DISCOVER_RUNNING) {
            lv_obj_remove_flag(s_ov_hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(s_ov_hint, lv_color_hex(COL_MUTED), 0);
            lv_label_set_text(s_ov_hint, L("正在查找小电拼…", "Searching for charger..."));
        } else if (ds == POWER_DISCOVER_FAILED) {
            lv_obj_remove_flag(s_ov_hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(s_ov_hint, lv_color_hex(COL_RED), 0);
            lv_label_set_text(s_ov_hint,
                              L("未找到小电拼\n请在设置里填写 IP", "Charger not found\nSet IP in Settings"));
        } else {
            lv_obj_add_flag(s_ov_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    uint32_t total_mw = 0;
    for (int i = 0; i < POWER_METRICS_MAX_PORTS; i++) {
        total_mw += power_port_milliwatts(&view->metrics.ports[i]);
    }

    char buf[24];
    if (view->poll_ok == 0 && view->state != POWER_LINK_LIVE) {
        lv_obj_set_style_text_font(s_ov_total, &lv_font_montserrat_44, 0);
        lv_label_set_text(s_ov_total, "--");
    } else {
        power_format_watts(total_mw, buf, sizeof(buf));
        char text[32];
        snprintf(text, sizeof(text), "%sW", buf);
        // 0.00W / 123.4W 这类长字符串用 36px，避免超出卡片和右侧占用徽标。
        const lv_font_t *font =
            strlen(text) >= 5 ? &lv_font_montserrat_36 : &lv_font_montserrat_44;
        lv_obj_set_style_text_font(s_ov_total, font, 0);
        lv_label_set_text(s_ov_total, text);
    }

    // 160W 总预算的占用百分比：pct10 以 0.1% 为单位（1% = 1600mW）。
    if (view->poll_ok == 0 && view->state != POWER_LINK_LIVE) {
        lv_label_set_text(s_ov_usage, "--");
        lv_obj_set_width(s_ov_usage_bar, 3);
    } else {
        uint32_t pct10 = total_mw / 160u;
        if (pct10 > 1000) pct10 = 1000;

        char usage[16];
        if (pct10 >= 100) {
            snprintf(usage, sizeof(usage), "%u%%", (unsigned)(pct10 / 10));
        } else {
            snprintf(usage, sizeof(usage), "%u.%u%%", (unsigned)(pct10 / 10),
                     (unsigned)(pct10 % 10));
        }
        lv_label_set_text(s_ov_usage, usage);

        int bar_w = (int)(pct10 * 56 / 1000);
        if (bar_w < 3) bar_w = 3;
        lv_obj_set_width(s_ov_usage_bar, bar_w);
    }

    lv_obj_set_style_border_width(s_ov_total_card, s_selected < 0 ? 2 : 0, 0);
    lv_obj_set_style_border_color(s_ov_total_card, lv_color_hex(COL_AMBER), 0);

    // 只有新数据或选中项变化时才重排行样式，避免每 200ms 无效重绘。
    const bool rows_dirty = (view->seq != s_ov_seq_seen) || (s_selected != s_ov_sel_seen);
    if (rows_dirty) {
        for (int i = 0; i < MAX_ROWS; i++) {
            const bool exists = i < view->metrics.port_count;
            const power_port_metrics_t *port = &view->metrics.ports[i];
            const bool active = exists && power_port_active(port);
            const bool selected = (i == s_selected);
            const bool out_off = view->port_on_valid[i] && !view->port_on[i];

            lv_obj_set_style_bg_color(s_ov_row[i], lv_color_hex(selected ? COL_CARD_SEL : COL_CARD), 0);
            lv_obj_set_style_bg_color(s_ov_bar[i], lv_color_hex(selected ? COL_AMBER : COL_CARD), 0);
            lv_obj_set_style_text_color(s_ov_name[i], lv_color_hex(exists ? COL_TEXT : COL_DIM), 0);

            if (out_off) {
                lv_label_set_text(s_ov_w[i], "OFF");
                lv_obj_set_style_text_color(s_ov_w[i], lv_color_hex(COL_RED), 0);
                lv_label_set_text(s_ov_va[i], L("已关闭", "output off"));
                lv_obj_set_style_text_color(s_ov_va[i], lv_color_hex(COL_DIM), 0);
            } else if (active) {
                char wbuf[16];
                power_format_watts(power_port_milliwatts(port), wbuf, sizeof(wbuf));
                char wtext[24];
                snprintf(wtext, sizeof(wtext), "%sW", wbuf);
                lv_label_set_text(s_ov_w[i], wtext);
                lv_obj_set_style_text_color(s_ov_w[i], lv_color_hex(COL_AMBER), 0);

                char v[12];
                char a[12];
                power_format_volts(port->voltage_mv, v, sizeof(v));
                power_format_amps(port->current_ma, a, sizeof(a));
                char va[32];
                snprintf(va, sizeof(va), "%sV  %sA", v, a);
                lv_label_set_text(s_ov_va[i], va);
                lv_obj_set_style_text_color(s_ov_va[i], lv_color_hex(COL_MUTED), 0);
            } else {
                lv_label_set_text(s_ov_w[i], "--");
                lv_obj_set_style_text_color(s_ov_w[i], lv_color_hex(COL_DIM), 0);
                lv_label_set_text(s_ov_va[i], exists ? L("待机", "idle") : L("无接口", "no port"));
                lv_obj_set_style_text_color(s_ov_va[i], lv_color_hex(COL_DIM), 0);
            }
        }
        s_ov_seq_seen = view->seq;
        s_ov_sel_seen = s_selected;
    }
}

static uint32_t history_at(const power_link_view_t *view, int port, int index) {
    const int len = view->history_len;
    if (len <= 0) return 0;
    const int start = (view->history_head - len + POWER_HISTORY_LEN) % POWER_HISTORY_LEN;
    const int slot = (start + index) % POWER_HISTORY_LEN;
    return port < 0 ? view->total_history[slot] : view->port_history[port][slot];
}

// 设备在拿不到线材能力时会返回 "unknown(N)" 这类原始枚举值。
static bool value_unknown(const char *value) {
    if (!value || value[0] == '\0') return true;
    return strncasecmp(value, "unknown", 7) == 0;
}

// 解析 "unknown(N)" 里的 N；不是这种形式返回 -1。
static int unknown_enum_index(const char *value) {
    if (!value) return -1;
    const char *p = strchr(value, '(');
    return p ? atoi(p + 1) : -1;
}

// 线材最大 VBUS 电压/电流：优先用设备给的可读值，否则按原始枚举还原
// (0/1/2/3 -> 20/30/40/50V；1/2 -> 3/5A)。
static void cable_rating(const power_pd_info_t *pd, int *volts, int *amps) {
    static const int voltages[] = {20, 30, 40, 50};
    static const int currents[] = {0, 3, 5};

    *volts = 0;
    *amps = 0;

    if (!value_unknown(pd->cable_max_v)) {
        *volts = atoi(pd->cable_max_v);
    } else {
        const int idx = unknown_enum_index(pd->cable_max_v);
        if (idx >= 0 && idx < (int)(sizeof(voltages) / sizeof(voltages[0]))) *volts = voltages[idx];
    }

    if (!value_unknown(pd->cable_max_a)) {
        *amps = atoi(pd->cable_max_a);
    } else {
        const int idx = unknown_enum_index(pd->cable_max_a);
        if (idx >= 0 && idx < (int)(sizeof(currents) / sizeof(currents[0]))) *amps = currents[idx];
    }
}

// USB 速率拆成“名称 + 带宽”两段，例如 "USB 2.0" -> "USB2.0" / "480Mbps"。
static void speed_parts(const char *speed, char *name, size_t name_len, char *rate, size_t rate_len) {
    name[0] = '\0';
    rate[0] = '\0';
    if (value_unknown(speed)) return;

    size_t n = 0;
    for (const char *p = speed; *p && n + 1 < name_len; p++) {
        if (*p != ' ') name[n++] = *p;
    }
    name[n] = '\0';

    const char *text = NULL;
    if (strstr(speed, "USB4 Gen4")) text = "80Gbps";
    else if (strstr(speed, "USB4 Gen3")) text = "40Gbps";
    else if (strstr(speed, "Gen2")) text = "10Gbps";
    else if (strstr(speed, "Gen1")) text = "5Gbps";
    else if (strstr(speed, "USB 2.0")) text = "480Mbps";

    if (text) snprintf(rate, rate_len, "%s", text);
}

// 把设备返回的长协议名压缩成适合一行显示的名称。
static const char *short_protocol(const char *protocol, char *buf, size_t len) {
    if (!protocol || protocol[0] == '\0') return "PD";
    if (strstr(protocol, "Programmable Power Supply")) snprintf(buf, len, "PD PPS");
    else if (strstr(protocol, "Fixed High Voltage")) snprintf(buf, len, "PD Fixed");
    else if (strstr(protocol, "Fixed 5V")) snprintf(buf, len, "PD 5V");
    else if (strstr(protocol, "Legacy")) buf[0] = '\0';
    else if (strstr(protocol, "Not Charging")) snprintf(buf, len, L("未充电", "Idle"));
    else {
        snprintf(buf, len, "%s", protocol);
        if (strlen(buf) > 16) buf[16] = '\0';
    }
    return buf;
}

static void chart_reload(const power_link_view_t *view, int port) {
    int32_t raw[POWER_HISTORY_LEN];
    int32_t values[POWER_HISTORY_LEN];
    const int len = view->history_len;
    const int n = s_chart_points;
    for (int i = 0; i < n; i++) {
        const int src = len - n + i;
        raw[i] = src >= 0 ? (int32_t)(history_at(view, port, src) / 100) : 0;
        values[i] = 0;
    }

    // 三点滑动平均，压掉采样噪声，让趋势更清楚。
    for (int i = 0; i < n; i++) {
        if (i == 0 && n > 0 && len - n + i < 0) continue;
        int32_t sum = 0;
        int cnt = 0;
        for (int k = -1; k <= 1; k++) {
            const int j = i + k;
            if (j < 0 || j >= n) continue;
            if (len - n + j < 0) continue;
            sum += raw[j];
            cnt++;
        }
        values[i] = cnt > 0 ? sum / cnt : raw[i];
    }

    int32_t vmin = INT32_MAX;
    int32_t vmax = INT32_MIN;
    for (int i = 0; i < n; i++) {
        if (len - n + i < 0) continue;
        if (values[i] < vmin) vmin = values[i];
        if (values[i] > vmax) vmax = values[i];
    }

    // 自动缩放 Y 轴：波动小时放大，避免曲线看起来是一条直线。
    int32_t lo = 0;
    int32_t hi = 100;
    if (vmax >= vmin) {
        const int32_t span = vmax - vmin;
        if (span < 20) {
            const int32_t mid = (vmax + vmin) / 2;
            lo = mid - 10;
            hi = mid + 10;
        } else {
            const int32_t pad = span / 4 + 5;
            lo = vmin - pad;
            hi = vmax + pad;
        }
        if (lo < 0) lo = 0;
        if (hi - lo < 10) hi = lo + 10;
    }
    lv_chart_set_axis_range(s_dt_chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
    lv_chart_set_series_values(s_dt_chart, s_dt_series, values, (uint32_t)n);
    s_chart_seq = view->seq;
}

static void update_detail(const power_link_view_t *view) {
    const int port = s_detail_port;
    char title[24];
    snprintf(title, sizeof(title), L("接口 %s", "PORT %s"), PORT_LABELS[port]);
    lv_label_set_text(s_dt_port, title);

    const power_port_metrics_t *p = &view->metrics.ports[port];
    const bool active = power_port_active(p);
    if (active) {
        char wbuf[16];
        power_format_watts(power_port_milliwatts(p), wbuf, sizeof(wbuf));
        char wtext[24];
        snprintf(wtext, sizeof(wtext), "%sW", wbuf);
        lv_label_set_text(s_dt_w, wtext);

        char v[12];
        char a[12];
        power_format_volts(p->voltage_mv, v, sizeof(v));
        power_format_amps(p->current_ma, a, sizeof(a));
        char va[48];
        // 不再显示温度：Montserrat 14 没有“·”字形，会渲染成方框。
        snprintf(va, sizeof(va), "%sV %sA", v, a);
        lv_label_set_text(s_dt_va, va);
    } else {
        lv_label_set_text(s_dt_w, "--");
        lv_label_set_text(s_dt_va, L("未接入设备", "nothing plugged in"));
    }

    // 协议放在功率卡里，信息卡留给线材。
    const power_pd_info_t *pd = &view->pd[port];
    char proto[40];
    const char *proto_src = (pd->valid && pd->protocol[0]) ? pd->protocol : p->protocol;
    const char *short_name = short_protocol(proto_src, proto, sizeof(proto));
    if (pd->valid) {
        lv_label_set_text(s_dt_proto, short_name);
        lv_obj_set_style_text_color(s_dt_proto, lv_color_hex(COL_TEXT), 0);
    } else if (proto_src[0]) {
        lv_label_set_text(s_dt_proto, short_name);
        lv_obj_set_style_text_color(s_dt_proto, lv_color_hex(COL_MUTED), 0);
    } else {
        lv_label_set_text(s_dt_proto, "");
    }

    // 端口输出状态：开关只由云端 MCP 提供，局域网轮询时保留上次已知值。
    if (view->ctl_state == POWER_CTL_BUSY && view->ctl_port == port + 1) {
        lv_label_set_text(s_dt_out, L("输出: 切换中", "OUT: SWITCHING"));
        lv_obj_set_style_text_color(s_dt_out, lv_color_hex(COL_AMBER), 0);
    } else if (view->ctl_state == POWER_CTL_FAIL && view->ctl_port == port + 1) {
        lv_label_set_text(s_dt_out, L("输出: 失败", "OUT: FAILED"));
        lv_obj_set_style_text_color(s_dt_out, lv_color_hex(COL_RED), 0);
    } else if (view->port_on_valid[port]) {
        lv_label_set_text(s_dt_out, view->port_on[port] ? L("输出: 开", "OUT: ON") : L("输出: 关", "OUT: OFF"));
        lv_obj_set_style_text_color(s_dt_out,
                                    lv_color_hex(view->port_on[port] ? COL_GREEN : COL_RED), 0);
    } else {
        lv_label_set_text(s_dt_out, L("输出: ?", "OUT: ?"));
        lv_obj_set_style_text_color(s_dt_out, lv_color_hex(COL_DIM), 0);
    }

    // 信息卡（两列三行）：USB 名称 / 带宽、Vol / Cur、VID / 值
    char line[56];
    if (pd->valid) {
        int volts = 0;
        int amps = 0;
        cable_rating(pd, &volts, &amps);

        char speed_name[16];
        char speed_rate[12];
        speed_parts(pd->cable_speed, speed_name, sizeof(speed_name), speed_rate, sizeof(speed_rate));

        lv_label_set_text(s_dt_info[0], speed_name[0] ? speed_name : "USB --");
        lv_label_set_text(s_dt_info[1], speed_rate);

        if (volts > 0) {
            snprintf(line, sizeof(line), "Vol:%dV", volts);
        } else {
            snprintf(line, sizeof(line), "Vol:--");
        }
        lv_label_set_text(s_dt_info[2], line);

        if (amps > 0) {
            snprintf(line, sizeof(line), "Cur:%dA", amps);
        } else {
            snprintf(line, sizeof(line), "Cur:--");
        }
        lv_label_set_text(s_dt_info[3], line);

        lv_label_set_text(s_dt_info[4], "VID");
        if (pd->cable_vid != 0) {
            snprintf(line, sizeof(line), "%04X", pd->cable_vid);
        } else {
            snprintf(line, sizeof(line), "----");
        }
        lv_label_set_text(s_dt_info[5], line);

        for (int i = 0; i < 6; i++) {
            lv_obj_set_style_text_color(s_dt_info[i], lv_color_hex(COL_TEXT), 0);
        }
    } else {
        lv_label_set_text(s_dt_info[0], "USB --");
        lv_label_set_text(s_dt_info[1], "");
        lv_label_set_text(s_dt_info[2], "Vol:--");
        lv_label_set_text(s_dt_info[3], "Cur:--");
        lv_label_set_text(s_dt_info[4], "VID");
        if (pd->tried) {
            lv_label_set_text(s_dt_info[5], "----");
        } else if (s_have_cfg && s_cfg.mcp_url[0]) {
            lv_label_set_text(s_dt_info[5], "...");
        } else {
            lv_label_set_text(s_dt_info[5], "cloud");
        }
        for (int i = 0; i < 6; i++) {
            lv_obj_set_style_text_color(s_dt_info[i], lv_color_hex(COL_DIM), 0);
        }
    }

    lv_label_set_text(s_dt_chart_title, L("最近 60 秒", "LAST 60 s"));
    s_chart_points = 60000 / (int)s_ui_poll_ms;
    if (s_chart_points < 1) s_chart_points = 1;
    if (s_chart_points > POWER_HISTORY_LEN) s_chart_points = POWER_HISTORY_LEN;
    if (lv_chart_get_point_count(s_dt_chart) != (uint32_t)s_chart_points) {
        lv_chart_set_point_count(s_dt_chart, (uint32_t)s_chart_points);
    }

    if (view->seq != s_chart_seq) chart_reload(view, port);
}

static void settings_value_text(int idx, char *out, size_t len, const power_link_view_t *view) {
    const bool cloud = s_have_cfg &&
                       (s_cfg.mode == APP_LINK_CLOUD ||
                        (s_cfg.mode == APP_LINK_AUTO && view->via_cloud));

    out[0] = '\0';
    switch (idx) {
    case SET_ID_WIFI:
        snprintf(out, len, "%s", s_have_cfg ? s_cfg.ssid : L("未配置", "not configured"));
        break;
    case SET_ID_IP:
        snprintf(out, len, "%s", view->ip[0] ? view->ip : "--");
        break;
    case SET_ID_SIGNAL:
        if (view->rssi_valid) snprintf(out, len, "%d dBm", view->rssi);
        else snprintf(out, len, "--");
        break;
    case SET_ID_RATE:
        if (s_ui_poll_ms == 500) snprintf(out, len, "%s", L("快 0.5s", "FAST 0.5s"));
        else if (s_ui_poll_ms == 3000) snprintf(out, len, "%s", L("慢 3s", "SLOW 3s"));
        else snprintf(out, len, "%s", L("中 1s", "NORMAL 1s"));
        break;
    case SET_ID_OFFTIME:
        if (s_screen_off_ms == 0) snprintf(out, len, "%s", L("常亮", "ALWAYS ON"));
        else snprintf(out, len, "%d min", (int)(s_screen_off_ms / 60000));
        break;
    case SET_ID_BRIGHT:
        snprintf(out, len, "%u%%", (unsigned)s_brightness);
        break;
    case SET_ID_LANG:
        snprintf(out, len, "%s", L("中文", "English"));
        break;
    case SET_ID_IDLE:
        snprintf(out, len, "%s", s_ui_idle_pause ? L("暂停轮询", "PAUSE") : L("继续轮询", "KEEP"));
        break;
    case SET_ID_RESET:
        snprintf(out, len, ">");
        break;
    default:
        break;
    }
}

static void update_settings(const power_link_view_t *view) {
    if (s_set_cursor < s_set_top) s_set_top = s_set_cursor;
    if (s_set_cursor >= s_set_top + SET_VISIBLE) s_set_top = s_set_cursor - SET_VISIBLE + 1;
    if (s_set_top > SET_ITEM_COUNT - SET_VISIBLE) s_set_top = SET_ITEM_COUNT - SET_VISIBLE;
    if (s_set_top < 0) s_set_top = 0;

    for (int i = 0; i < SET_VISIBLE; i++) {
        const int idx = s_set_top + i;
        if (idx >= SET_ITEM_COUNT) {
            lv_obj_add_flag(s_set_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_set_row[i], LV_OBJ_FLAG_HIDDEN);

        const bool cursor = (idx == s_set_cursor);
        const bool editing = cursor && s_set_edit;
        const uint32_t bg = cursor ? (s_set_edit ? COL_SET_EDIT : COL_CARD_SEL) : COL_BG;
        lv_obj_set_style_bg_color(s_set_row[i], lv_color_hex(bg), 0);
        lv_obj_set_style_text_color(s_set_name[i],
                                    lv_color_hex(cursor ? COL_TEXT : COL_MUTED), 0);
        lv_obj_set_style_text_color(s_set_value[i],
                                    lv_color_hex(cursor ? COL_AMBER : COL_TEXT), 0);
        (void)editing;

        lv_label_set_text(s_set_name[i], s_lang_zh ? SET_ITEMS[idx].zh : SET_ITEMS[idx].en);
        char value[40];
        settings_value_text(idx, value, sizeof(value), view);
        lv_label_set_text(s_set_value[i], value);
    }

    // 右上角显示“当前项/总数”，例如 1/11。
    char page[24];
    snprintf(page, sizeof(page), "%d/%d", s_set_cursor + 1, SET_ITEM_COUNT);
    lv_label_set_text(s_set_page, page);
}

static void settings_move(int delta) {
    s_set_cursor += delta;
    if (s_set_cursor < 0) s_set_cursor = SET_ITEM_COUNT - 1;
    if (s_set_cursor >= SET_ITEM_COUNT) s_set_cursor = 0;
    update_settings(view_refresh());
}

static void ui_brightness_step(int delta);
static void ui_rebuild(void);

static void settings_adjust(int delta) {
    switch (s_set_cursor) {
    case SET_ID_RATE: {
        static const uint16_t rates[] = {500, 1000, 3000};
        int index = 1;
        for (int i = 0; i < 3; i++) {
            if (rates[i] == s_ui_poll_ms) index = i;
        }
        index = (index + delta + 3) % 3;
        s_ui_poll_ms = rates[index];
        (void)app_config_set_poll_ms(s_ui_poll_ms);
        power_link_set_poll_ms(s_ui_poll_ms);
        break;
    }
    case SET_ID_BRIGHT:
        ui_brightness_step(delta);
        break;
    case SET_ID_LANG:
        repeat_stop();
        s_lang_zh = !s_lang_zh;
        (void)app_config_set_lang(s_lang_zh ? 1 : 0);
        s_set_edit = false;
        ui_rebuild();
        return;
    case SET_ID_IDLE:
        s_ui_idle_pause = !s_ui_idle_pause;
        (void)app_config_set_idle_pause(s_ui_idle_pause ? 1 : 0);
        power_link_set_idle_pause(s_ui_idle_pause);
        break;
    case SET_ID_OFFTIME: {
        static const uint32_t offs[] = {60000, 180000, 300000, 0};
        int index = 2;
        for (int i = 0; i < 4; i++) {
            if (offs[i] == s_screen_off_ms) index = i;
        }
        index = (index + delta + 4) % 4;
        s_screen_off_ms = offs[index];
        (void)app_config_set_screen_off_ms(s_screen_off_ms);
        break;
    }
    default:
        break;
    }
    update_settings(view_refresh());
}


// ---------------------------------------------------------------- 选项弹窗

typedef struct {
    const char *zh;
    const char *en;
} popup_opt_t;

static const popup_opt_t P_LANG[2] = {{"中文", "Chinese"}, {"English", "English"}};
static const popup_opt_t P_RATE[3] = {{"快 0.5s", "FAST 0.5s"}, {"中 1s", "NORMAL 1s"}, {"慢 3s", "SLOW 3s"}};
static const popup_opt_t P_OFF[4] = {{"1 分钟", "1 min"}, {"3 分钟", "3 min"}, {"5 分钟", "5 min"}, {"常亮", "ALWAYS ON"}};
static const popup_opt_t P_BRIGHT[4] = {{"20%", "20%"}, {"50%", "50%"}, {"80%", "80%"}, {"100%", "100%"}};
static const popup_opt_t P_IDLE[2] = {{"暂停轮询", "PAUSE"}, {"保持轮询", "KEEP POLLING"}};

static const popup_opt_t *popup_opts(int item, int *count) {
    switch (item) {
    case SET_ID_LANG: *count = 2; return P_LANG;
    case SET_ID_RATE: *count = 3; return P_RATE;
    case SET_ID_OFFTIME: *count = 4; return P_OFF;
    case SET_ID_BRIGHT: *count = 4; return P_BRIGHT;
    case SET_ID_IDLE: *count = 2; return P_IDLE;
    default: *count = 0; return NULL;
    }
}

static int popup_current_index(int item) {
    switch (item) {
    case SET_ID_LANG: return s_lang_zh ? 0 : 1;
    case SET_ID_RATE: return s_ui_poll_ms == 500 ? 0 : (s_ui_poll_ms == 3000 ? 2 : 1);
    case SET_ID_OFFTIME:
        if (s_screen_off_ms == 60000) return 0;
        if (s_screen_off_ms == 180000) return 1;
        if (s_screen_off_ms == 300000) return 2;
        return 3;
    case SET_ID_BRIGHT: return s_brightness == 20 ? 0 : (s_brightness == 50 ? 1 : (s_brightness == 80 ? 2 : 3));
    case SET_ID_IDLE: return s_ui_idle_pause ? 0 : 1;
    default: return 0;
    }
}

static void popup_render(void) {
    int count = 0;
    const popup_opt_t *opts = popup_opts(s_popup_item, &count);
    s_popup_count = count;
    lv_label_set_text(s_popup_title, s_lang_zh ? SET_ITEMS[s_popup_item].zh : SET_ITEMS[s_popup_item].en);
    lv_obj_add_flag(s_popup_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_popup_opt[0], lv_color_hex(COL_TEXT), 0);
    for (int i = 0; i < 6; i++) {
        if (i >= count) {
            lv_obj_add_flag(s_popup_opt[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_popup_opt[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_popup_opt[i], s_lang_zh ? opts[i].zh : opts[i].en);
        const bool cur = (i == s_popup_cursor);
        lv_obj_set_style_text_color(s_popup_opt[i],
                                    lv_color_hex(cur ? COL_AMBER : COL_MUTED), 0);
    }
}

static void popup_open(int item) {
    s_popup_item = item;
    s_popup_cursor = popup_current_index(item);
    s_popup_open = true;
    popup_render();
    lv_obj_remove_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
}

static void popup_close(void) {
    s_popup_open = false;
    lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
}

static void popup_apply(void) {
    switch (s_popup_item) {
    case SET_ID_LANG:
        s_lang_zh = (s_popup_cursor == 0);
        (void)app_config_set_lang(s_lang_zh ? 1 : 0);
        popup_close();
        ui_rebuild();
        break;
    case SET_ID_RATE: {
        static const uint16_t rates[] = {500, 1000, 3000};
        s_ui_poll_ms = rates[s_popup_cursor];
        (void)app_config_set_poll_ms(s_ui_poll_ms);
        power_link_set_poll_ms(s_ui_poll_ms);
        break;
    }
    case SET_ID_OFFTIME: {
        static const uint32_t offs[] = {60000, 180000, 300000, 0};
        s_screen_off_ms = offs[s_popup_cursor];
        (void)app_config_set_screen_off_ms(s_screen_off_ms);
        break;
    }
    case SET_ID_BRIGHT: {
        static const uint8_t levels[] = {20, 50, 80, 100};
        s_brightness = levels[s_popup_cursor];
        if (!s_screen_off) bsp_display_backlight(s_brightness);
        (void)app_config_set_brightness(s_brightness);
        break;
    }
    case SET_ID_IDLE:
        s_ui_idle_pause = (s_popup_cursor == 0);
        (void)app_config_set_idle_pause(s_ui_idle_pause ? 1 : 0);
        power_link_set_idle_pause(s_ui_idle_pause);
        break;
    default:
        break;
    }
    popup_close();
    update_settings(view_refresh());
}

static void settings_activate(void) {
    const set_kind_t kind = SET_ITEMS[s_set_cursor].kind;
    if (kind == SET_CHOICE) {
        popup_open(s_set_cursor);
    } else if (s_set_cursor == SET_ID_WIFI) {
        wifi_picker_open();
    } else if (s_set_cursor == SET_ID_RESET) {
        s_confirm_reset = true;
        if (s_set_footer) lv_obj_add_flag(s_set_footer, LV_OBJ_FLAG_HIDDEN);
        if (s_set_footer2) lv_obj_add_flag(s_set_footer2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_confirm_box, LV_OBJ_FLAG_HIDDEN);
    }
}

// 亮度档位：20 / 50 / 80 / 100，UP 变亮、DOWN 变暗，写入 NVS。
static void ui_brightness_step(int delta) {
    static const uint8_t levels[] = {20, 50, 80, 100};
    int index = 3;
    for (int i = 0; i < 4; i++) {
        if (levels[i] == s_brightness) index = i;
    }
    index += delta;
    if (index < 0) index = 0;
    if (index > 3) index = 3;

    s_brightness = levels[index];
    if (!s_screen_off) bsp_display_backlight(s_brightness);
    (void)app_config_set_brightness(s_brightness);
    if (s_screen == SCREEN_SETTINGS) update_settings(view_refresh());
}

// 息屏/唤醒：只关背光并暂停刷新，数据轮询继续（唤醒即是最新值）。
static void display_panel_sleep(bool sleep) {
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!panel) return;
    if (sleep) {
        (void)esp_lcd_panel_disp_on_off(panel, false);
        (void)esp_lcd_panel_disp_sleep(panel, true);
    } else {
        (void)esp_lcd_panel_disp_sleep(panel, false);
        vTaskDelay(pdMS_TO_TICKS(120));  // ST7789 SLPOUT 后需要 120ms 才能显示
        (void)esp_lcd_panel_disp_on_off(panel, true);
    }
}

static void ui_screen_set(bool on) {
    s_screen_off = !on;
    power_link_set_idle(!on);
    if (!on) {
        bsp_display_backlight(0);
        display_panel_sleep(true);
        return;
    }

    display_panel_sleep(false);
    bsp_display_backlight(s_brightness);
    lv_obj_invalidate(lv_screen_active());
    s_last_activity_ms = esp_timer_get_time() / 1000;

    const power_link_view_t *view = view_refresh();
    switch (s_screen) {
    case SCREEN_OVERVIEW:
        update_overview(view);
        break;
    case SCREEN_DETAIL:
        update_detail(view);
        break;
    case SCREEN_TOTAL:
        update_total(view);
        break;
    case SCREEN_SETTINGS:
        update_settings(view);
        break;
    default:
        break;
    }
}

static void ui_tick(lv_timer_t *timer) {
    (void)timer;

    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (!s_screen_off && s_screen_off_ms > 0 && now_ms - s_last_activity_ms >= (int64_t)s_screen_off_ms) {
        ui_screen_set(false);
    }
    if (s_screen_off) return;  // 息屏后不再刷新界面

    // 详情页每 30 秒刷新一次 PD/线材信息。
    if (s_screen == SCREEN_DETAIL && s_have_cfg && s_cfg.mcp_url[0]) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_pd_req_ms > 30000) {
            if (power_link_request_pd(s_detail_port + 1) == ESP_OK) s_pd_req_ms = now_ms;
        }
    }

    if (s_ov_battery) {
        if (s_battery_percent >= 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", (int)s_battery_percent);
            lv_label_set_text(s_ov_battery, buf);
        } else {
            lv_label_set_text(s_ov_battery, "");
        }
    }

    if (!s_pd_selftest && now_ms > 8000) {
        s_pd_selftest = true;
        ESP_LOGI(TAG, "自检: 请求 PD");
        (void)power_link_request_pd(2);
    }

    static uint32_t mem_log_tick;
    if (++mem_log_tick >= 150) {
        mem_log_tick = 0;
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGI(TAG, "LVGL 池空闲 %u, 最大块 %u, 碎片 %u%%",
                 (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
                 (unsigned)mon.frag_pct);
    }

    const power_link_view_t *view = view_refresh();

    switch (s_screen) {
    case SCREEN_OVERVIEW:
        update_overview(view);
        break;
    case SCREEN_DETAIL:
        update_detail(view);
        break;
    case SCREEN_TOTAL:
        update_total(view);
        break;
    case SCREEN_SETTINGS:
        update_settings(view);
        break;
    case SCREEN_WIFI:
        wifi_render();
        break;
    case SCREEN_PROVISIONING:
    default:
        break;
    }
}

static void load_screen(screen_t screen) {
    lv_obj_t *target = NULL;
    switch (screen) {
    case SCREEN_OVERVIEW: target = s_scr_overview; break;
    case SCREEN_DETAIL: target = s_scr_detail; break;
    case SCREEN_TOTAL: target = s_scr_total; break;
    case SCREEN_SETTINGS: target = s_scr_settings; break;
    case SCREEN_PROVISIONING: target = s_scr_provision; break;
    case SCREEN_WIFI: target = s_scr_wifi; break;
    case SCREEN_KEYBOARD: target = s_scr_kbd; break;
    case SCREEN_NUMERIC: target = s_scr_num; break;
    default: break;
    }
    if (!target) {
        screen = SCREEN_OVERVIEW;
        target = s_scr_overview;
    }
    if (!target) return;

    repeat_stop();
    s_screen = screen;
    switch (screen) {
    case SCREEN_OVERVIEW:
        lv_screen_load(s_scr_overview);
        break;
    case SCREEN_DETAIL:
        lv_screen_load(s_scr_detail);
        break;
    case SCREEN_TOTAL:
        lv_screen_load(s_scr_total);
        break;
    case SCREEN_SETTINGS:
        lv_screen_load(s_scr_settings);
        break;
    case SCREEN_PROVISIONING:
        lv_screen_load(s_scr_provision);
        break;
    case SCREEN_WIFI:
        lv_screen_load(s_scr_wifi);
        break;
    case SCREEN_KEYBOARD:
        lv_screen_load(s_scr_kbd);
        break;
    case SCREEN_NUMERIC:
        lv_screen_load(s_scr_num);
        break;
    }
    // 切换页面后强制整屏重绘，避免残留上一页的文字或边框。
    lv_obj_invalidate(lv_screen_active());
}

// 切换语言后重建全部界面（保留当前页面）。调用方必须已持有 LVGL 锁。
static void ui_rebuild(void) {
    const screen_t current = s_screen;

    if (s_scr_overview) {
        lv_obj_del(s_scr_overview);
        s_scr_overview = NULL;
    }
    if (s_scr_detail) {
        lv_obj_del(s_scr_detail);
        s_scr_detail = NULL;
    }
    if (s_scr_total) {
        lv_obj_del(s_scr_total);
        s_scr_total = NULL;
    }
    if (s_scr_settings) {
        lv_obj_del(s_scr_settings);
        s_scr_settings = NULL;
    }
    if (s_scr_provision) {
        lv_obj_del(s_scr_provision);
        s_scr_provision = NULL;
    }
    if (s_scr_wifi) {
        lv_obj_del(s_scr_wifi);
        s_scr_wifi = NULL;
    }
    if (s_scr_kbd) {
        lv_obj_del(s_scr_kbd);
        s_scr_kbd = NULL;
    }
    if (s_scr_num) {
        lv_obj_del(s_scr_num);
        s_scr_num = NULL;
    }

    s_confirm_output = false;
    s_confirm_reset = false;
    s_chart_seq = UINT32_MAX;
    s_ov_seq_seen = UINT32_MAX;
    s_ov_sel_seen = -1;

    build_overview();
    build_detail();
    build_total();
    build_settings();
    build_provisioning();
    build_wifi();
    build_keyboard();

    load_screen(current);
    const power_link_view_t *view = view_refresh();
    switch (s_screen) {
    case SCREEN_OVERVIEW:
        update_overview(view);
        break;
    case SCREEN_DETAIL:
        update_detail(view);
        break;
    case SCREEN_SETTINGS:
        update_settings(view);
        break;
    default:
        break;
    }
}

void app_ui_show_overview(void) {
    if (!bsp_lvgl_lock(200)) return;
    load_screen(SCREEN_OVERVIEW);
    update_overview(view_refresh());
    bsp_lvgl_unlock();
}

void app_ui_show_provisioning(const char *ap_ssid) {
    if (!bsp_lvgl_lock(200)) return;
    if (s_pv_ssid && ap_ssid) lv_label_set_text(s_pv_ssid, ap_ssid);
    load_screen(SCREEN_PROVISIONING);
    bsp_lvgl_unlock();
}

static void open_detail(int port) {
    s_confirm_output = false;
    lv_obj_add_flag(s_out_confirm, LV_OBJ_FLAG_HIDDEN);
    s_detail_port = port < 0 ? 0 : (port >= POWER_METRICS_MAX_PORTS ? POWER_METRICS_MAX_PORTS - 1 : port);
    s_chart_seq = UINT32_MAX;
    load_screen(SCREEN_DETAIL);
    update_detail(view_refresh());
    if (s_have_cfg && s_cfg.mcp_url[0]) {
        if (power_link_request_pd(s_detail_port + 1) == ESP_OK) {
            s_pd_req_ms = esp_timer_get_time() / 1000;
        }
    }
}

static void open_settings(void) {
    s_confirm_reset = false;
    lv_obj_add_flag(s_confirm_box, LV_OBJ_FLAG_HIDDEN);
    load_screen(SCREEN_SETTINGS);
    update_settings(view_refresh());
}

static bool cursor_available(void) {
    if (s_screen == SCREEN_SETTINGS) return !s_confirm_reset;
    if (s_screen == SCREEN_WIFI) return !s_scan_busy && s_ap_count > 0;
    if (s_screen == SCREEN_KEYBOARD) return true;
    if (s_screen == SCREEN_NUMERIC) return true;
    return false;
}

static void cursor_move(int dir) {
    switch (s_screen) {
    case SCREEN_SETTINGS:
        if (s_confirm_reset) return;
        if (s_popup_open) {
            s_popup_cursor += dir;
            if (s_popup_cursor < 0) s_popup_cursor = 0;
            if (s_popup_cursor >= s_popup_count) s_popup_cursor = s_popup_count - 1;
            popup_render();
        } else if (s_set_edit) {
            settings_adjust(dir);
        } else {
            settings_move(dir);
        }
        break;
    case SCREEN_NUMERIC:
        numeric_move(dir);
        break;
    case SCREEN_WIFI:
        if (s_scan_busy || s_ap_count <= 0) return;
        s_ap_cursor += dir;
        if (s_ap_cursor < 0) s_ap_cursor = 0;
        if (s_ap_cursor >= s_ap_count) s_ap_cursor = s_ap_count - 1;
        wifi_render();
        break;
    case SCREEN_KEYBOARD:
        kbd_move(dir);
        break;
    default:
        break;
    }
}

static void repeat_timer_cb(lv_timer_t *timer) {
    (void)timer;
    cursor_move(s_repeat_dir);
}

static void repeat_stop(void) {
    if (s_repeat_timer) {
        lv_timer_del(s_repeat_timer);
        s_repeat_timer = NULL;
    }
}

static void repeat_start(void) {
    // 连发已移除，避免菜单误加速。
}

void app_ui_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_screen == SCREEN_PROVISIONING) return;
    if (!bsp_lvgl_lock(200)) return;

    s_last_activity_ms = esp_timer_get_time() / 1000;

    // 息屏时任意功能键唤醒；唤醒后的短窗口内丢弃按键，避免顺带触发导航。
    if (s_screen_off) {
        if (ev == BSP_BTN_PRESS || ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) {
            ui_screen_set(true);
            s_wake_ignore_until_ms = esp_timer_get_time() / 1000 + WAKE_IGNORE_MS;
        }
        bsp_lvgl_unlock();
        return;
    }
    if (esp_timer_get_time() / 1000 < s_wake_ignore_until_ms) {
        bsp_lvgl_unlock();
        return;
    }

    if (ev == BSP_BTN_PRESS) {
        if ((btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) && cursor_available()) {
            s_suppress_click = true;
            s_repeat_dir = (btn == BSP_BTN_UP) ? -1 : 1;
            cursor_move(s_repeat_dir);
            repeat_start();
        }
        bsp_lvgl_unlock();
        return;
    }
    if (ev == BSP_BTN_RELEASE) {
        repeat_stop();
        bsp_lvgl_unlock();
        return;
    }
    if (ev == BSP_BTN_LONG) {
        repeat_stop();
    }
    if (ev == BSP_BTN_CLICK && s_suppress_click &&
        (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        s_suppress_click = false;
        bsp_lvgl_unlock();
        return;
    }

    // 双击 OK 手动息屏/唤醒（硬件电源键不接 MCU，无法做短按检测）。
    if (ev == BSP_BTN_DOUBLE && btn == BSP_BTN_OK) {
        if (s_screen == SCREEN_KEYBOARD) {
            kbd_backspace();
            bsp_lvgl_unlock();
            return;
        }
        if (s_screen == SCREEN_NUMERIC) {
            numeric_backspace();
            bsp_lvgl_unlock();
            return;
        }
        if (s_screen == SCREEN_OVERVIEW || s_screen == SCREEN_DETAIL) {
            ui_screen_set(false);
            bsp_lvgl_unlock();
            return;
        }
    }

    if (s_screen == SCREEN_OVERVIEW) {
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            if (s_selected < 0) s_selected = MAX_ROWS - 1;
            else s_selected--;
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            if (s_selected >= MAX_ROWS - 1) s_selected = -1;
            else s_selected++;
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            if (s_selected < 0) open_total();
            else open_detail(s_selected);
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            open_settings();
        }
    } else if (s_screen == SCREEN_TOTAL) {
        if (s_confirm_all) {
            if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
                (void)power_link_control_all(s_toggle_on);
                s_confirm_all = false;
                lv_obj_add_flag(s_tt_confirm, LV_OBJ_FLAG_HIDDEN);
            } else if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) {
                s_confirm_all = false;
                lv_obj_add_flag(s_tt_confirm, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            const power_link_view_t *view = view_refresh();
            bool any_off = false;
            for (int i = 0; i < MAX_ROWS; i++) {
                if (view->port_on_valid[i] && !view->port_on[i]) any_off = true;
            }
            s_toggle_on = any_off;
            lv_label_set_text(s_tt_confirm_title,
                              any_off ? L("开启所有接口？", "TURN ALL PORTS ON?")
                                      : L("关闭所有接口？", "TURN ALL PORTS OFF?"));
            s_confirm_all = true;
            lv_obj_remove_flag(s_tt_confirm, LV_OBJ_FLAG_HIDDEN);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            load_screen(SCREEN_OVERVIEW);
        }
    } else if (s_screen == SCREEN_DETAIL) {
        if (s_confirm_output) {
            if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
                const power_link_view_t *view = view_refresh();
                const bool known = view->port_on_valid[s_detail_port];
                const bool target = known ? !view->port_on[s_detail_port] : true;
                s_confirm_output = false;
                lv_obj_add_flag(s_out_confirm, LV_OBJ_FLAG_HIDDEN);
                const esp_err_t err = power_link_control(s_detail_port + 1, target);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "端口控制请求失败: %s", esp_err_to_name(err));
                }
            } else if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) {
                s_confirm_output = false;
                lv_obj_add_flag(s_out_confirm, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            if (!s_have_cfg || s_cfg.mcp_url[0] == '\0') {
                ESP_LOGW(TAG, "未配置云端 MCP，无法控制端口");
            } else {
                const power_link_view_t *view = view_refresh();
                const bool known = view->port_on_valid[s_detail_port];
                const bool on = known ? view->port_on[s_detail_port] : true;
                char title[24];
                snprintf(title, sizeof(title), "%s %s？", on ? "关闭" : "打开",
                         PORT_LABELS[s_detail_port]);
                lv_label_set_text(s_out_confirm_title, title);
                s_confirm_output = true;
                lv_obj_remove_flag(s_out_confirm, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            load_screen(SCREEN_OVERVIEW);
        }
    } else if (s_screen == SCREEN_WIFI) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            load_screen(SCREEN_SETTINGS);
            update_settings(view_refresh());
        } else if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            if (s_ap_count > 0) {
                s_ap_cursor += (btn == BSP_BTN_UP ? -1 : 1);
                if (s_ap_cursor < 0) s_ap_cursor = 0;
                if (s_ap_cursor >= s_ap_count) s_ap_cursor = s_ap_count - 1;
                wifi_render();
            }
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            if (s_ap_count == 0) {
                wifi_scan_start();
                wifi_render();
            } else {
                wifi_pick_current();
            }
        }
    } else if (s_screen == SCREEN_KEYBOARD) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            load_screen(SCREEN_WIFI);
            wifi_render();
        } else if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            kbd_move(btn == BSP_BTN_UP ? -1 : 1);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            kbd_apply_key();
        }
    } else if (s_screen == SCREEN_NUMERIC) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            load_screen(SCREEN_SETTINGS);
            update_settings(view_refresh());
        } else if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            numeric_move(btn == BSP_BTN_UP ? -1 : 1);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            numeric_press(s_num_cursor);
            numeric_render();
        }
    } else if (s_screen == SCREEN_SETTINGS) {
        if (s_confirm_reset) {
            if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                ESP_LOGW(TAG, "恢复出厂并重启");
                (void)app_config_factory_reset();
                bsp_lvgl_unlock();
                esp_restart();
                return;
            }
            if (ev == BSP_BTN_CLICK) {
                s_confirm_reset = false;
                lv_obj_add_flag(s_confirm_box, LV_OBJ_FLAG_HIDDEN);
                if (s_set_footer) lv_obj_remove_flag(s_set_footer, LV_OBJ_FLAG_HIDDEN);
                if (s_set_footer2) lv_obj_remove_flag(s_set_footer2, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (s_popup_open) {
            if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
                s_popup_cursor += (btn == BSP_BTN_UP ? -1 : 1);
                if (s_popup_cursor < 0) s_popup_cursor = 0;
                if (s_popup_cursor >= s_popup_count) s_popup_cursor = s_popup_count - 1;
                popup_render();
            } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
                popup_apply();
            } else if (ev == BSP_BTN_LONG || ev == BSP_BTN_CLICK) {
                popup_close();
                update_settings(view_refresh());
            }
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            load_screen(SCREEN_OVERVIEW);
        } else if (s_set_edit) {
            if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
                settings_adjust(btn == BSP_BTN_UP ? 1 : -1);
            } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
                s_set_edit = false;
                update_settings(view_refresh());
            }
        } else if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            settings_move(btn == BSP_BTN_UP ? -1 : 1);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            settings_activate();
        }
    }
    bsp_lvgl_unlock();
}

void app_ui_init(const app_config_t *cfg) {
    s_lang_zh = app_config_get_lang() != 0;
    // Montserrat 缺中文字形，复制一份并挂思源黑体回退。
    memcpy(&s_font14, &lv_font_montserrat_14, sizeof(lv_font_t));
    s_font14.fallback = &ui_font_14;
    memcpy(&s_font20, &lv_font_montserrat_20, sizeof(lv_font_t));
    s_font20.fallback = &ui_font_20;
    s_have_cfg = cfg != NULL;
    if (cfg) s_cfg = *cfg;
    s_brightness = app_config_get_brightness();
    s_ui_poll_ms = app_config_get_poll_ms();
    s_ui_idle_pause = app_config_get_idle_pause() != 0;
    s_screen_off_ms = app_config_get_screen_off_ms();
    s_screen = SCREEN_OVERVIEW;
    s_selected = 0;
    s_detail_port = 0;

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL 加锁失败，界面未创建");
        return;
    }

    build_overview();
    build_detail();
    build_total();
    build_settings();
    build_provisioning();
    build_wifi();
    build_keyboard();

    s_last_activity_ms = esp_timer_get_time() / 1000;
    s_screen_off = false;
    s_wake_ignore_until_ms = 0;
    s_scan_task = xTaskCreateStatic(wifi_scan_task, "wifiscan", 3072, NULL, 2,
                                    s_scan_stack, &s_scan_tcb);
    if (!s_scan_task) ESP_LOGW(TAG, "扫描任务创建失败");
    lv_timer_create(ui_tick, 200, NULL);
    load_screen(SCREEN_OVERVIEW);
    bsp_display_backlight(s_brightness);
    bsp_lvgl_unlock();

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL 池: 空闲 %u 字节, 最大块 %u, 碎片 %u%%, 堆空闲 %u",
             (unsigned)mon.free_size, (unsigned)mon.free_biggest_size, (unsigned)mon.frag_pct,
             (unsigned)esp_get_free_heap_size());

    if (xTaskCreate(battery_task, "battery", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "电池读取任务创建失败");
    }
}

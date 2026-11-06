// main/link_failover.h —— AUTO 模式的双通道切换状态机(纯逻辑，可主机测试)。
//
// 规则：
//   1. 默认优先局域网；
//   2. 局域网失败（重连后首次探测失败 1 次即切；平时连续 2 次）且配置了云端 -> 切云端；
//   3. 云端连续 2 次失败且配置了局域网 -> 回到局域网；云端每 5 分钟慢速回探一次局域网。
//   4. 任一通道成功即清零该通道的失败计数。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LINK_FAILOVER_THRESHOLD 2
#define LINK_LAN_PROBE_MS (5 * 60 * 1000)

typedef enum {
    LINK_SWITCH_NONE = 0,
    LINK_SWITCH_TO_CLOUD,
    LINK_SWITCH_TO_LAN,
    LINK_SWITCH_PROBE_LAN,
} link_switch_t;

typedef struct {
    bool poll_cloud;   // 当前实际使用的通道
    int lan_fail;
    int cloud_fail;
    int64_t lan_probe_ms;  // 预留字段，当前不再安排回探
    bool lan_first_probe;  // Wi-Fi 重连后的首次局域网探测：失败一次即切云端
} link_failover_t;

// prefer_lan=false 时从云端通道开始(只配了云端，或强制云端)。
void link_failover_init(link_failover_t *st, bool prefer_lan);

// 每次轮询结束后调用。返回发生的切换动作，供上层打日志。
link_switch_t link_failover_on_result(link_failover_t *st, bool ok, bool has_lan, bool has_cloud,
                                      int64_t now_ms);

// 每次轮询循环末尾调用，用于到点回探局域网。
link_switch_t link_failover_tick(link_failover_t *st, bool has_lan, int64_t now_ms);

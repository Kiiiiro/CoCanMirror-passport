#include "link_failover.h"

#include <stddef.h>

void link_failover_init(link_failover_t *st, bool prefer_lan) {
    if (!st) return;
    st->poll_cloud = !prefer_lan;
    st->lan_fail = 0;
    st->cloud_fail = 0;
    st->lan_probe_ms = 0;
    st->lan_first_probe = prefer_lan;
}

link_switch_t link_failover_on_result(link_failover_t *st, bool ok, bool has_lan, bool has_cloud,
                                      int64_t now_ms) {
    if (!st) return LINK_SWITCH_NONE;

    if (ok) {
        if (st->poll_cloud) {
            st->cloud_fail = 0;
        } else {
            st->lan_fail = 0;
            st->lan_first_probe = false;
        }
        return LINK_SWITCH_NONE;
    }

    if (st->poll_cloud) {
        st->cloud_fail++;
    } else {
        st->lan_fail++;
    }

    const int lan_threshold = st->lan_first_probe ? 1 : LINK_FAILOVER_THRESHOLD;
    if (!st->poll_cloud && st->lan_fail >= lan_threshold && has_cloud) {
        st->poll_cloud = true;
        st->lan_fail = 0;
        st->cloud_fail = 0;
        st->lan_first_probe = false;
        st->lan_probe_ms = now_ms + LINK_LAN_PROBE_MS;  // 5 分钟后慢速回探一次
        return LINK_SWITCH_TO_CLOUD;
    }

    if (st->poll_cloud && st->cloud_fail >= LINK_FAILOVER_THRESHOLD && has_lan) {
        st->poll_cloud = false;
        st->lan_fail = 0;
        st->cloud_fail = 0;
        st->lan_probe_ms = 0;
        return LINK_SWITCH_TO_LAN;
    }

    return LINK_SWITCH_NONE;
}

link_switch_t link_failover_tick(link_failover_t *st, bool has_lan, int64_t now_ms) {
    if (!st || !st->poll_cloud || !has_lan) return LINK_SWITCH_NONE;
    if (st->lan_probe_ms <= 0 || now_ms < st->lan_probe_ms) return LINK_SWITCH_NONE;

    st->poll_cloud = false;
    st->lan_fail = 0;
    st->lan_first_probe = true;  // 回探失败一次就退回云端
    st->lan_probe_ms = 0;
    return LINK_SWITCH_PROBE_LAN;
}

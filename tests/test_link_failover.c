#include <assert.h>

#include "link_failover.h"

static void test_prefer_lan_until_three_failures(void) {
    link_failover_t st;
    link_failover_init(&st, true);
    assert(!st.poll_cloud);

    assert(link_failover_on_result(&st, false, true, true, 1000) == LINK_SWITCH_NONE);
    assert(link_failover_on_result(&st, false, true, true, 2000) == LINK_SWITCH_NONE);
    assert(link_failover_on_result(&st, false, true, true, 3000) == LINK_SWITCH_TO_CLOUD);
    assert(st.poll_cloud);
    assert(st.lan_probe_ms == 3000 + LINK_LAN_PROBE_MS);
}

static void test_success_resets_counter(void) {
    link_failover_t st;
    link_failover_init(&st, true);

    assert(link_failover_on_result(&st, false, true, true, 1000) == LINK_SWITCH_NONE);
    assert(link_failover_on_result(&st, false, true, true, 2000) == LINK_SWITCH_NONE);
    assert(link_failover_on_result(&st, true, true, true, 3000) == LINK_SWITCH_NONE);
    assert(st.lan_fail == 0);

    // 重新计三次才切换。
    assert(link_failover_on_result(&st, false, true, true, 4000) == LINK_SWITCH_NONE);
    assert(link_failover_on_result(&st, false, true, true, 5000) == LINK_SWITCH_NONE);
    assert(link_failover_on_result(&st, false, true, true, 6000) == LINK_SWITCH_TO_CLOUD);
}

static void test_no_cloud_keeps_lan(void) {
    link_failover_t st;
    link_failover_init(&st, true);

    for (int i = 0; i < 10; i++) {
        assert(link_failover_on_result(&st, false, true, false, i * 1000) == LINK_SWITCH_NONE);
    }
    assert(!st.poll_cloud);
}

static void test_cloud_failure_returns_to_lan(void) {
    link_failover_t st;
    link_failover_init(&st, false);  // 只有云端
    assert(st.poll_cloud);

    assert(link_failover_on_result(&st, false, true, true, 1000) == LINK_SWITCH_NONE);
    assert(link_failover_on_result(&st, false, true, true, 2000) == LINK_SWITCH_NONE);
    assert(link_failover_on_result(&st, false, true, true, 3000) == LINK_SWITCH_TO_LAN);
    assert(!st.poll_cloud);
    assert(st.lan_probe_ms == 0);
}

static void test_probe_after_two_minutes(void) {
    link_failover_t st;
    link_failover_init(&st, true);

    link_failover_on_result(&st, false, true, true, 0);
    link_failover_on_result(&st, false, true, true, 0);
    assert(link_failover_on_result(&st, false, true, true, 0) == LINK_SWITCH_TO_CLOUD);

    // 回探时间还没到。
    assert(link_failover_tick(&st, true, LINK_LAN_PROBE_MS - 1) == LINK_SWITCH_NONE);
    assert(st.poll_cloud);

    assert(link_failover_tick(&st, true, LINK_LAN_PROBE_MS) == LINK_SWITCH_PROBE_LAN);
    assert(!st.poll_cloud);
    assert(st.lan_probe_ms == 0);
}

static void test_probe_needs_lan_configured(void) {
    link_failover_t st;
    link_failover_init(&st, false);

    assert(link_failover_tick(&st, false, 10 * LINK_LAN_PROBE_MS) == LINK_SWITCH_NONE);
    assert(st.poll_cloud);
}

int main(void) {
    test_prefer_lan_until_three_failures();
    test_success_resets_counter();
    test_no_cloud_keeps_lan();
    test_cloud_failure_returns_to_lan();
    test_probe_after_two_minutes();
    test_probe_needs_lan_configured();
    return 0;
}

#include <assert.h>
#include <string.h>

#include "power_metrics.h"

static const char SAMPLE[] =
    "# HELP ionbridge_port_voltage Port voltage in mV\r\n"
    "# TYPE ionbridge_port_voltage gauge\r\n"
    "ionbridge_port_voltage{id=\"0\"} 5000\r\n"
    "ionbridge_port_current{id=\"0\"} 2500\r\n"
    "ionbridge_port_voltage{id=\"1\"} 20000\r\n"
    "ionbridge_port_current{id=\"1\"} 2240\r\n"
    "ionbridge_port_voltage{id=\"2\"} 9000\r\n"
    "ionbridge_port_current{id=\"2\"} 0\r\n"
    "ionbridge_port_voltage{id=\"3\"} 0\r\n"
    "ionbridge_port_current{id=\"3\"} 0\r\n"
    "ionbridge_uptime_seconds 3725\r\n"
    "ionbridge_wifi_signal{ssid=\"cocan\",channel=\"6\"} -52\r\n";

static void test_parse_sample(void) {
    power_snapshot_t snap;
    const int ports = power_metrics_parse(SAMPLE, strlen(SAMPLE), &snap);
    assert(ports == 4);
    assert(snap.port_count == 4);

    assert(snap.ports[0].has_voltage && snap.ports[0].has_current);
    assert(snap.ports[0].voltage_mv == 5000);
    assert(snap.ports[0].current_ma == 2500);
    assert(power_port_milliwatts(&snap.ports[0]) == 12500);
    assert(power_port_active(&snap.ports[0]));

    assert(power_port_milliwatts(&snap.ports[1]) == 44800);
    assert(power_port_active(&snap.ports[1]));

    // 有电压、无电流：设备在位但没取电(例如已充满)，仍算在位。
    assert(power_port_milliwatts(&snap.ports[2]) == 0);
    assert(power_port_active(&snap.ports[2]));

    // 电压与电流都为 0：接口空置。
    assert(!power_port_active(&snap.ports[3]));

    assert(snap.uptime_valid && snap.uptime_s == 3725);
    assert(snap.rssi_valid && snap.rssi_dbm == -52);
}

static void test_parse_without_labels(void) {
    static const char text[] =
        "ionbridge_port_voltage 12000\n"
        "ionbridge_port_current 1000\n";
    power_snapshot_t snap;
    assert(power_metrics_parse(text, strlen(text), &snap) == 1);
    assert(snap.ports[0].voltage_mv == 12000);
    assert(power_port_milliwatts(&snap.ports[0]) == 12000);
}

static void test_parse_rejects_garbage(void) {
    power_snapshot_t snap;
    static const char text[] = "hello 1\n{\nnot a metric\n";
    assert(power_metrics_parse(text, strlen(text), &snap) == -1);

    static const char empty[] = "";
    assert(power_metrics_parse(empty, 0, &snap) == -1);
}

static void test_formats(void) {
    char buf[16];
    power_format_watts(5670, buf, sizeof(buf));
    assert(strcmp(buf, "5.67") == 0);
    power_format_watts(45800, buf, sizeof(buf));
    assert(strcmp(buf, "45.8") == 0);
    power_format_watts(128000, buf, sizeof(buf));
    assert(strcmp(buf, "128") == 0);
    power_format_watts(0, buf, sizeof(buf));
    assert(strcmp(buf, "0.00") == 0);

    power_format_volts(20100, buf, sizeof(buf));
    assert(strcmp(buf, "20.1") == 0);
    power_format_volts(5000, buf, sizeof(buf));
    assert(strcmp(buf, "5.0") == 0);

    power_format_amps(2240, buf, sizeof(buf));
    assert(strcmp(buf, "2.24") == 0);
    power_format_amps(850, buf, sizeof(buf));
    assert(strcmp(buf, "0.85") == 0);
}

int main(void) {
    test_parse_sample();
    test_parse_without_labels();
    test_parse_rejects_garbage();
    test_formats();
    return 0;
}

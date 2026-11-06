#include "power_metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static bool has_prefix(const char *s, size_t len, const char *prefix) {
    const size_t n = strlen(prefix);
    return len >= n && memcmp(s, prefix, n) == 0;
}

// 在标签串中查找 `key="<digits>"`；也接受不带引号的形式。
static bool find_label_number(const char *labels, size_t len, const char *key, int *out) {
    const size_t key_len = strlen(key);
    for (size_t i = 0; i + key_len < len; i++) {
        if (memcmp(labels + i, key, key_len) != 0) continue;
        size_t p = i + key_len;
        if (p < len && labels[p] == '=') {
            p++;
        } else {
            continue;
        }
        if (p < len && (labels[p] == '"' || labels[p] == '\'')) p++;
        if (p >= len || labels[p] < '0' || labels[p] > '9') continue;
        int value = 0;
        while (p < len && labels[p] >= '0' && labels[p] <= '9') {
            if (value < 100000) value = value * 10 + (labels[p] - '0');
            p++;
        }
        *out = value;
        return true;
    }
    return false;
}

static bool parse_value(const char *s, size_t len, double *out) {
    char tmp[64];
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < sizeof(tmp); i++) {
        if (is_blank(s[i])) break;
        tmp[n++] = s[i];
    }
    tmp[n] = '\0';
    if (n == 0) return false;
    char *end = NULL;
    const double value = strtod(tmp, &end);
    if (end == tmp) return false;
    *out = value;
    return true;
}

static uint32_t clamp_u32(double value) {
    if (value <= 0.0) return 0;
    if (value >= 4294967295.0) return UINT32_MAX;
    return (uint32_t)(value + 0.5);
}

static int32_t clamp_i32(double value) {
    if (value <= -2147483648.0) return INT32_MIN;
    if (value >= 2147483647.0) return INT32_MAX;
    return (int32_t)value;
}

int power_metrics_parse(const char *text, size_t len, power_snapshot_t *out) {
    if (!text || !out) return -1;
    memset(out, 0, sizeof(*out));

    bool found = false;
    const char *p = text;
    const char *end = text + len;

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        const char *line = p;
        size_t line_len = (size_t)(line_end - p);
        p = line_end < end ? line_end + 1 : end;

        while (line_len > 0 && is_blank(line[0])) {
            line++;
            line_len--;
        }
        while (line_len > 0 && is_blank(line[line_len - 1])) line_len--;
        if (line_len == 0 || line[0] == '#') continue;

        // 指标名
        size_t name_len = 0;
        while (name_len < line_len && line[name_len] != '{' && !is_blank(line[name_len])) name_len++;

        // 可选标签 {...}
        const char *labels = NULL;
        size_t labels_len = 0;
        size_t cursor = name_len;
        if (cursor < line_len && line[cursor] == '{') {
            const char *close = memchr(line + cursor, '}', line_len - cursor);
            if (!close) continue;
            labels = line + cursor + 1;
            labels_len = (size_t)(close - labels);
            cursor = (size_t)(close - line) + 1;
        }
        while (cursor < line_len && is_blank(line[cursor])) cursor++;
        double value = 0.0;
        if (!parse_value(line + cursor, line_len - cursor, &value)) continue;

        int port_id = -1;
        if (labels && !find_label_number(labels, labels_len, "id", &port_id)) {
            (void)find_label_number(labels, labels_len, "port", &port_id);
        }
        // 单接口设备可能省略标签；带标签但缺少接口号的端口指标会在下面被跳过。
        if (!labels && port_id < 0) port_id = 0;

        if (has_prefix(line, name_len, "ionbridge_port_voltage")) {
            if (port_id < 0 || port_id >= POWER_METRICS_MAX_PORTS) continue;
            out->ports[port_id].has_voltage = true;
            out->ports[port_id].voltage_mv = clamp_u32(value);
            found = true;
        } else if (has_prefix(line, name_len, "ionbridge_port_current")) {
            if (port_id < 0 || port_id >= POWER_METRICS_MAX_PORTS) continue;
            out->ports[port_id].has_current = true;
            out->ports[port_id].current_ma = clamp_u32(value);
            found = true;
        } else if (has_prefix(line, name_len, "ionbridge_uptime_seconds")) {
            out->uptime_valid = true;
            out->uptime_s = value > 0 ? (uint64_t)value : 0;
            found = true;
        } else if (has_prefix(line, name_len, "ionbridge_wifi_signal")) {
            out->rssi_valid = true;
            out->rssi_dbm = clamp_i32(value);
            found = true;
        }
    }

    for (int i = 0; i < POWER_METRICS_MAX_PORTS; i++) {
        if (out->ports[i].has_voltage || out->ports[i].has_current) {
            out->port_count = i + 1;
        }
    }
    if (!found) return -1;
    return out->port_count > 0 ? out->port_count : 1;
}

uint32_t power_port_milliwatts(const power_port_metrics_t *port) {
    if (!port || !port->has_voltage || !port->has_current) return 0;
    const uint64_t mw = (uint64_t)port->voltage_mv * (uint64_t)port->current_ma / 1000u;
    return mw > UINT32_MAX ? UINT32_MAX : (uint32_t)mw;
}

bool power_port_active(const power_port_metrics_t *port) {
    if (!port) return false;
    if (port->has_current && port->current_ma > 0) return true;
    return port->has_voltage && port->voltage_mv > 0;
}

void power_format_watts(uint32_t milliwatts, char *buf, size_t len) {
    if (!buf || len == 0) return;
    const unsigned long mw = milliwatts;
    if (mw < 10000ul) {
        snprintf(buf, len, "%lu.%02lu", mw / 1000ul, (mw % 1000ul) / 10ul);
    } else if (mw < 100000ul) {
        snprintf(buf, len, "%lu.%lu", mw / 1000ul, (mw % 1000ul) / 100ul);
    } else {
        snprintf(buf, len, "%lu", (mw + 500ul) / 1000ul);
    }
}

void power_format_volts(uint32_t millivolts, char *buf, size_t len) {
    if (!buf || len == 0) return;
    const unsigned long mv = millivolts;
    snprintf(buf, len, "%lu.%lu", mv / 1000ul, (mv % 1000ul) / 100ul);
}

void power_format_amps(uint32_t milliamps, char *buf, size_t len) {
    if (!buf || len == 0) return;
    const unsigned long ma = milliamps;
    snprintf(buf, len, "%lu.%02lu", ma / 1000ul, (ma % 1000ul) / 10ul);
}

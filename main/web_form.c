#include "web_form.h"

#include <string.h>

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void web_form_decode(char *s) {
    if (!s) return;
    char *out = s;
    for (char *in = s; *in; in++) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (*in == '%' && in[1] && in[2]) {
            const int hi = hex_value(in[1]);
            const int lo = hex_value(in[2]);
            if (hi >= 0 && lo >= 0) {
                *out++ = (char)((hi << 4) | lo);
                in += 2;
            } else {
                *out++ = *in;
            }
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

bool web_form_get(const char *body, const char *key, char *out, size_t out_len) {
    if (!body || !key || !out || out_len == 0) return false;
    out[0] = '\0';

    const size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        const size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        if (pair_len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *value = p + key_len + 1;
            size_t value_len = pair_len - key_len - 1;
            if (value_len >= out_len) value_len = out_len - 1;
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            web_form_decode(out);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

esp_err_t web_send_html(httpd_req_t *req, const char *body) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

void web_html_escape(const char *src, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t n = 0;
    for (const char *p = src; *p && n + 1 < dst_len; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        if (rep) {
            const size_t len = strlen(rep);
            if (n + len >= dst_len) break;
            memcpy(dst + n, rep, len);
            n += len;
        } else {
            dst[n++] = *p;
        }
    }
    dst[n] = '\0';
}

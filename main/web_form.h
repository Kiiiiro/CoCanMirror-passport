// main/web_form.h —— 两个配置页(SoftAP 首配 / 局域网运行时)共用的表单工具。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

// 就地把 application/x-www-form-urlencoded 的值解码(%XX 与 '+')。
void web_form_decode(char *s);

// 从表单体里取字段；不存在返回 false(输出为空串)。
bool web_form_get(const char *body, const char *key, char *out, size_t out_len);

// 发送一段 HTML，带 UTF-8 与 no-store。
esp_err_t web_send_html(httpd_req_t *req, const char *body);

// 把文本转义后写入 dst，用于把配置值放回 HTML 属性。
void web_html_escape(const char *src, char *dst, size_t dst_len);

// main/lan_discover.h —— 局域网里查找小电拼（以 /metrics 内容为准）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 在当前 STA 网段内扫描 80 端口并请求 /metrics，命中 ionbridge_ 特征即返回。
// 找到返回 true 并把点分十进制地址写入 ip_out；budget_ms 为总时间预算。
bool lan_discover_find(char *ip_out, size_t ip_out_len, uint32_t budget_ms);

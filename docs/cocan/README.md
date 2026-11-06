# CoCanMirror Power Monitor（AI Passport 固件）

把 FoloToy AI Passport 变成小电拼（CoCan）充电器的随身功率仪表：通过云端 MCP 读取总输出、各接口电压电流与快充协议，显示 60 秒功率曲线，并可在设备上直接开关每个接口。

## 功能

- 总览：总输出功率、160W 占用比例、5 个接口的实时电压/电流/功率
- 接口详情：功率、电压电流、快充协议、最近 60 秒曲线
- 总输出详情：总功率与占比、各接口状态（OFF / 待机 / 输出中）、60 秒曲线
- 接口开关：详情页长按 OK 切换单口；总输出页长按 OK 按当前状态开启/关闭全部接口
- 配网：首次开机进入 `AIPassport-XXXX` 热点，手机自动弹出配置页（或访问 `http://192.168.4.1`），从列表选择 Wi-Fi、填写小电拼 MCP 地址
- 设置：轮询速度（0.5 / 1 / 3 秒）、息屏轮询（暂停 / 保持）、息屏时间、亮度、中英文
- 息屏：背光与屏幕显示同时关闭，面板进入睡眠；唤醒时先唤醒面板再恢复显示

## 目录

- `main/`：应用代码（UI、云端 MCP、配网、配置存储）
- `components/`：BSP 与驱动
- `firmware/`：可刷写的整机固件与校验值
- `docs/cocan/`：本应用的说明

## 构建

需要 ESP-IDF 5.5.3：

```bash
idf.py set-target esp32c3
idf.py build
```

整机镜像（从 0x0 写入）：

```bash
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 firmware/FoloToy-AI-Passport-2.6.4-public.bin
```

## 已知限制

- 仅支持云端 MCP，需要联网和有效 MCP 地址；不再支持局域网直连
- 线缆芯片详情（USB 速率/线材规格/VID）暂不显示：服务端单次返回约 12KB，超出设备 8KB 缓冲
- 云端 TLS 当前为加密但不校验证书，建议连接可信 Wi-Fi

## 致谢

基于 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 开发，部分设计思路参考 CP02 电源监控项目。

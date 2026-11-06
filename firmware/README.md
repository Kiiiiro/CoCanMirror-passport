# 固件

- `FoloToy-AI-Passport-2.6.3-public.bin`：整机镜像，从 0x0 写入
- `SHA256SUMS.txt`：校验值

刷写：

```bash
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 FoloToy-AI-Passport-2.6.3-public.bin
```

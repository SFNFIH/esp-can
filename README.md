# ESP-CAN (ESP32-S3)

基于 [ESP-IDF TWAI Network 示例](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/twai/twai_network) 的一对 CAN（TWAI）工程，目标芯片均为 **ESP32-S3**。

| 目录 | 角色 | 对应官方示例 |
|------|------|--------------|
| `esp-can-tx` | 发送端 | `twai_sender` |
| `esp-can-rx` | 接收端 | `twai_listen_only`（改为 normal 模式以便 ACK） |

## 硬件连接

1. 每块 ESP32-S3 接 CAN 收发器（如 SN65HVD230）
2. 两路收发器的 CAN_H / CAN_L 互联（总线拓扑）
3. 总线两端各接 120Ω 终端电阻

默认 GPIO（可用 `idf.py menuconfig` → Example Configuration 修改）：

- TX: GPIO4
- RX: GPIO5
- 波特率: 1 Mbps

## 编译烧录

需要已安装 ESP-IDF（建议 v5.5+，需新版 `esp_driver_twai` API）。

```bash
# 发送端
cd esp-can-tx
idf.py set-target esp32s3
idf.py build flash monitor

# 接收端（另一块板）
cd esp-can-rx
idf.py set-target esp32s3
idf.py build flash monitor
```

## 报文说明

| ID | 类型 | 频率 | 说明 |
|----|------|------|------|
| 0x7FF | Heartbeat | 1 Hz | 时间戳 |
| 0x100 | Data | 约每 10s | 1000 字节突发（125 帧） |
| 0x080 | Emergency | 突发期间 | 高优先级空帧 |

接收端硬件过滤器匹配 `0x100`（mask `0x7F0`），可收到 data 与相近 ID 帧。

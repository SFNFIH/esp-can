# ESP-CAN（ESP32-S3）

基于 ESP-IDF 的 **TWAI（CAN）+ ESP-NOW** 工程，用于从汽车 CAN 总线无线转发报文，并在车端提供网页实时查看。

| 工程 | 作用 |
|------|------|
| `esp-can-tx` | 连接汽车 CAN；ESP-NOW 转发；开启热点供手机/电脑网页监视 |
| `esp-can-rx` | 接收 ESP-NOW 封装后的 CAN 帧并打印（后续可再扩展） |

## 数据流

```
汽车 CAN 总线
    │
    ▼
esp-can-tx
    ├── ESP-NOW ──────────────► esp-can-rx
    └── SoftAP 网页监视
         手机/电脑连接热点
         打开 http://192.168.4.1/
```

## 网页监视（esp-can-tx）

1. 烧录并运行 `esp-can-tx`
2. 手机或电脑连接 Wi-Fi 热点：
   - 名称（SSID）：`ESP-CAN-TX`
   - 密码：`espcan123`
3. 浏览器打开：**http://192.168.4.1/**
4. 页面通过 WebSocket 实时显示 CAN 帧与统计信息

可在 `menuconfig` 中修改热点名称/密码。

## 硬件说明

### esp-can-tx（车端）

1. ESP32-S3 + CAN 收发器（如 SN65HVD230）
2. 收发器的 CAN_H / CAN_L 并入车辆 CAN  
   （注意安全与当地法规；默认只听模式，不发送 ACK）
3. 默认引脚：TX = GPIO4，RX = GPIO5
4. 默认波特率：**500000**（需与车辆一致，常见还有 250000）

### esp-can-rx（接收端）

- 仅需给 ESP32-S3 供电即可接收 ESP-NOW

## ESP-NOW 配对步骤

1. 先运行 `esp-can-rx`，串口打印本机 STA MAC  
2. 在 `esp-can-tx` 的 `menuconfig` 中填写对端 MAC  
   （或改 `sdkconfig.defaults` 里的 `CONFIG_EXAMPLE_ESPNOW_PEER_MAC`）
3. 两端信道必须相同（默认 1；与 SoftAP 共用）
4. 联调可用广播：`FF:FF:FF:FF:FF:FF`

## 编译与烧录

需要 ESP-IDF **v5.5+**（新版 `esp_driver_twai`，并启用 `CONFIG_HTTPD_WS_SUPPORT`）。

```bash
cd esp-can-rx
idf.py set-target esp32s3
idf.py build flash monitor

cd esp-can-tx
idf.py set-target esp32s3
idf.py build flash monitor
```

## ESP-NOW 数据格式

```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;    // 0xCA
    uint8_t  version;  // 0x01
    uint8_t  flags;    // bit0=扩展帧, bit1=RTR
    uint8_t  dlc;      // 0..8
    uint32_t id;
    uint32_t seq;
    uint8_t  data[8];
} can_espnow_frame_t;
```

头文件：`main/can_espnow_proto.h`（两工程各一份，保持相同）。

## 常用配置项

| 配置项 | 说明 | 默认 |
|--------|------|------|
| SoftAP SSID / 密码 | 网页监视热点 | ESP-CAN-TX / espcan123 |
| TWAI TX / RX GPIO | CAN 引脚 | 4 / 5 |
| TWAI bitrate | 波特率 | 500000 |
| Listen-only | 只听不 ACK | 开启 |
| Wi-Fi / ESP-NOW channel | 信道 | 1 |
| ESP-NOW peer MAC | RX 的 MAC | FF:FF:FF:FF:FF:FF |

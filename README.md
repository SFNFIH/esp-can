# ESP-CAN (ESP32-S3)

一对基于 ESP-IDF TWAI + ESP-NOW 的工程，用于从汽车 CAN 总线无线转发报文。

| 工程 | 角色 |
|------|------|
| `esp-can-tx` | 挂在汽车 CAN 上，接收 CAN 帧，经 ESP-NOW 发给 RX |
| `esp-can-rx` | 接收 ESP-NOW 封装的 CAN 帧并打印（可再扩展） |

## 数据流

```
汽车 CAN 总线
    │
    ▼
esp-can-tx  (TWAI listen-only, 默认 500 kbps)
    │  ESP-NOW (同信道)
    ▼
esp-can-rx  (当前：日志输出)
```

## 硬件

**esp-can-tx（车端）**

1. ESP32-S3 + CAN 收发器（如 SN65HVD230）
2. 收发器 CAN_H / CAN_L 并入车辆 CAN（注意安全与法规；默认 listen-only，不 ACK）
3. 默认 GPIO：TX=4，RX=5（`menuconfig` 可改）
4. 波特率默认 **500000**（与车辆匹配，常见还有 250000）

**esp-can-rx**

- 仅需 ESP32-S3 供电即可收 ESP-NOW（后续若要再出 CAN，再接收发器）

## ESP-NOW 配对

1. 先烧录并运行 `esp-can-rx`，串口会打印：
   `Local STA MAC (set this on esp-can-tx peer): AA:BB:CC:DD:EE:FF`
2. 在 `esp-can-tx` 中配置对端 MAC：
   ```bash
   idf.py menuconfig
   # ESP-CAN-TX Configuration → ESP-NOW peer MAC
   ```
   或改 `esp-can-tx/sdkconfig.defaults` 里的 `CONFIG_EXAMPLE_ESPNOW_PEER_MAC`
3. 两端 **Wi-Fi 信道必须一致**（默认 1）
4. 开发阶段可用广播 `FF:FF:FF:FF:FF:FF`（TX 默认）

## 编译烧录

需要 ESP-IDF **v5.5+**（新版 `esp_driver_twai`）。

```bash
# 接收端
cd esp-can-rx
idf.py set-target esp32s3
idf.py build flash monitor

# 车端网关
cd esp-can-tx
idf.py set-target esp32s3
idf.py build flash monitor
```

## ESP-NOW 载荷

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

头文件：`main/can_espnow_proto.h`（两工程各有一份，保持相同）。

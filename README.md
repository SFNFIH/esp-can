# ESP-CAN（ESP32-S3）

基于 ESP-IDF 的 **TWAI（CAN）+ ESP-NOW** 工程，用于从汽车 CAN 总线无线转发报文。

| 工程 | 作用 |
|------|------|
| `esp-can-tx` | 连接汽车 CAN，接收 CAN 帧，再通过 ESP-NOW 发给 RX |
| `esp-can-rx` | 接收 ESP-NOW 封装后的 CAN 帧并打印（后续可再扩展） |

## 数据流

```
汽车 CAN 总线
    │
    ▼
esp-can-tx  （TWAI 只听模式，默认 500 kbps）
    │  ESP-NOW（两端信道需一致）
    ▼
esp-can-rx  （当前：串口打印收到的帧）
```

## 硬件说明

### esp-can-tx（车端）

1. ESP32-S3 + CAN 收发器（如 SN65HVD230）
2. 收发器的 CAN_H / CAN_L 并入车辆 CAN  
   （注意安全与当地法规；默认只听模式，不发送 ACK）
3. 默认引脚：TX = GPIO4，RX = GPIO5（可在 `menuconfig` 中修改）
4. 默认波特率：**500000**（需与车辆一致，常见还有 250000）

### esp-can-rx（接收端）

- 仅需给 ESP32-S3 供电即可接收 ESP-NOW  
- 若以后还要再往本地 CAN 转发，再加收发器即可

## ESP-NOW 配对步骤

1. 先编译烧录并运行 `esp-can-rx`，串口会打印本机 STA MAC，例如：  
   `本机 STA MAC（请填到 esp-can-tx 的对端 MAC）：AA:BB:CC:DD:EE:FF`
2. 在 `esp-can-tx` 里配置对端 MAC：
   ```bash
   idf.py menuconfig
   # 进入：ESP-CAN-TX Configuration → ESP-NOW peer MAC
   ```
   也可直接改 `esp-can-tx/sdkconfig.defaults` 中的：
   ```
   CONFIG_EXAMPLE_ESPNOW_PEER_MAC="AA:BB:CC:DD:EE:FF"
   ```
3. 两端 **Wi-Fi 信道必须相同**（默认都是 1）
4. 联调阶段可先用广播地址：`FF:FF:FF:FF:FF:FF`（TX 工程默认值）

## 编译与烧录

需要 ESP-IDF **v5.5 及以上**（需新版 `esp_driver_twai`）。

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

## ESP-NOW 数据格式

两工程共用同一协议头（各自目录下的 `main/can_espnow_proto.h`，内容需保持一致）：

```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;    // 固定 0xCA
    uint8_t  version;  // 固定 0x01
    uint8_t  flags;    // bit0 = 扩展帧，bit1 = RTR
    uint8_t  dlc;      // 数据长度 0..8
    uint32_t id;       // CAN ID
    uint32_t seq;      // 序号，便于丢包/重复检测
    uint8_t  data[8];  // 数据区
} can_espnow_frame_t;
```

## 常用配置项（menuconfig）

| 配置项 | 说明 | 默认 |
|--------|------|------|
| TWAI TX / RX GPIO | CAN 收发器引脚 | 4 / 5 |
| TWAI bitrate | 车载 CAN 波特率 | 500000 |
| Listen-only | 只听、不 ACK | 开启 |
| ESP-NOW channel | Wi-Fi 信道 | 1 |
| ESP-NOW peer MAC | 对端（RX）MAC | FF:FF:FF:FF:FF:FF |

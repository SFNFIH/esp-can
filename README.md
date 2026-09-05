# ESP-CAN（ESP32-S3）

基于 ESP-IDF 的 **TWAI（CAN）+ ESP-NOW** 工程：从汽车 CAN 无线转发，并在车端提供网页实时查看。

| 工程 | 作用 |
|------|------|
| `esp-can-tx` | 连接汽车 CAN；ESP-NOW 转发；开启热点供手机/电脑网页监视 |
| `esp-can-rx` | 接收 ESP-NOW 封装后的 CAN 帧并打印 |

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
2. 手机或电脑连接 Wi-Fi：
   - 名称：`ESP-CAN-TX`
   - 密码：`espcan123`
3. 浏览器打开：**http://192.168.4.1/**
4. 页面每 200ms 拉取最新 CAN 帧，可暂停/清空/过滤扩展帧

热点名称与密码可在 `menuconfig` → **ESP-CAN-TX Configuration** 中修改。

## 硬件说明

### esp-can-tx（车端）

1. ESP32-S3 + CAN 收发器（如 SN65HVD230）
2. 收发器 CAN_H / CAN_L 并入车辆 CAN（注意安全与法规；默认只听，不 ACK）
3. 默认引脚：TX=GPIO4，RX=GPIO5
4. 默认波特率：**500000**

### esp-can-rx（接收端）

- 给 ESP32-S3 供电即可接收 ESP-NOW

## ESP-NOW 配对

1. 先运行 `esp-can-rx`，记下串口打印的 STA MAC  
2. 在 `esp-can-tx` 填写对端 MAC（或联调用广播 `FF:FF:FF:FF:FF:FF`）  
3. 两端信道一致（默认 1，与 SoftAP 共用）

## 编译烧录

需要 ESP-IDF **v5.5+**。

```bash
cd esp-can-rx && idf.py set-target esp32s3 && idf.py build flash monitor
cd esp-can-tx && idf.py set-target esp32s3 && idf.py build flash monitor
```

## 常用配置

| 配置项 | 默认 |
|--------|------|
| SoftAP SSID / 密码 | ESP-CAN-TX / espcan123 |
| TWAI TX/RX GPIO | 4 / 5 |
| TWAI 波特率 | 500000 |
| Listen-only | 开启 |
| Wi-Fi / ESP-NOW 信道 | 1 |
| ESP-NOW 对端 MAC | FF:FF:FF:FF:FF:FF |

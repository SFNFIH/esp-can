# ESP-CAN（ESP32-S3）

基于 ESP-IDF 的 **TWAI（CAN）+ ESP-NOW** 工程：从汽车 CAN 无线转发，车端可网页监视，接收端可用串口命令行查看/回传。两端按 FreeRTOS 任务管线拆分实时路径与慢路径。

| 工程 | 作用 |
|------|------|
| `esp-can-tx` | 连接汽车 CAN；ESP-NOW 转发；SoftAP 网页监视；可接收 RX 回传并注入 CAN |
| `esp-can-rx` | 接收 ESP-NOW CAN 帧；UART 命令行查看/过滤/回传 |

## 工程目录（ESP-IDF 标准布局）

每个子工程（`esp-can-tx` / `esp-can-rx`）结构如下：

```text
esp-can-tx/   （或 esp-can-rx/）
├── CMakeLists.txt
├── sdkconfig.defaults          # 默认配置（编译后生成 sdkconfig）
├── bootloader_components/      # 可选：自定义 bootloader 组件
├── components/                 # 本地组件
│   ├── can_espnow_proto/       # 共享协议头
│   │   ├── CMakeLists.txt
│   │   └── include/can_espnow_proto.h
│   ├── freertos_app/           # 任务优先级 / 事件位
│   │   ├── CMakeLists.txt
│   │   └── include/freertos_app.h
│   ├── web_monitor/            # 仅 tx：网页监视
│   │   ├── CMakeLists.txt
│   │   ├── Kconfig
│   │   ├── web_monitor.c
│   │   └── include/web_monitor.h
│   └── can_cli/                # 仅 rx：串口 CLI
│       ├── CMakeLists.txt
│       ├── Kconfig
│       ├── can_cli.c
│       └── include/can_cli.h
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── Kconfig.projbuild
│   └── main.c                  # app_main 入口
├── managed_components/         # 由组件管理器自动生成
├── dependencies.lock           # 由组件管理器自动生成
└── build/                      # 编译输出
```

`sdkconfig` / `dependencies.lock` / `managed_components/` / `build/` 由工具生成，已在 `.gitignore` 中忽略。

## 数据流

```
汽车 CAN 总线
    │
    ▼
esp-can-tx
    ├── ESP-NOW ──────────────► esp-can-rx（串口 CLI）
    │        ◄────────────────  can send 回传（可选注入总线）
    └── SoftAP 网页监视
         手机/电脑打开 http://192.168.4.1/
```

## FreeRTOS 任务模型

### esp-can-tx

| 机制 | 用途 |
|------|------|
| Task Notify（ISR→任务） | TWAI RX ISR 唤醒 `can_rx`，尽快腾出硬件缓冲 |
| Queue 扇出 | `can_rx` 同时投递 ESP-NOW / Web，互不阻塞 |
| Event Group | Wi-Fi / ESP-NOW / TWAI 就绪后再跑业务任务 |
| Soft Timer 1Hz | 刷新网页统计，避免单独占一个阻塞延时任务 |
| Binary Semaphore | 串行化 `esp_now_send` 与发送完成回调 |

优先级（高→低）：`can_rx` → `espnow_tx` → `web_feed` / `can_inject`

### esp-can-rx

| 机制 | 用途 |
|------|------|
| Queue + Task Notify | ESP-NOW 回调入队并唤醒 `can_rx` |
| Queue | CLI `can send` 入队，由 `can_tx` 真正发 ESP-NOW |
| Event Group | Wi-Fi / ESP-NOW 就绪；对端 MAC 学习置位 |
| Mutex | 保护帧环缓冲与 peer MAC |
| Soft Timer 1Hz | 心跳统计日志 |

优先级（高→低）：`can_rx` → CLI(REPL) → `can_tx`

## esp-can-rx 命令行

烧录后用串口监视器（115200）进入提示符 `esp-can-rx>`。

| 命令 | 说明 |
|------|------|
| `help` | 帮助 |
| `mac` | 显示本机 STA MAC |
| `peer` | 查看已学习/已设置的 TX MAC |
| `peer AA:BB:CC:DD:EE:FF` | 手动设置 TX MAC（用于回传） |
| `can stats` | 接收/回传统计 |
| `can last [n]` | 查看最近 n 帧（默认 20） |
| `can watch on\|off` | 实时打印收到的帧 |
| `can filter <id\|off>` | 按 ID 过滤，例如 `can filter 0x123` |
| `can clear` | 清空本地缓冲 |
| `can send [-e] <id> <hex...>` | 回传一帧给 TX（入队，由 `can_tx` 发送） |

示例：

```text
can watch on
can last 50
can filter 0x7E0
can send 0x7E0 02 10 01
can send -e 0x18DAF110 02 10 01
```

说明：

1. 收到 TX 帧后会自动学习对端 MAC；也可手动 `peer ...`
2. `can send` 需要 TX 关闭 listen-only，才会真正发到汽车 CAN  
   （`menuconfig` → `EXAMPLE_TWAI_LISTEN_ONLY` 取消勾选）

## esp-can-tx 网页监视

1. 连接 Wi-Fi：`ESP-CAN-TX` / `espcan123`
2. 浏览器打开：http://192.168.4.1/

## 硬件

### esp-can-tx

- ESP32-S3 + CAN 收发器（如 SN65HVD230）
- 默认 GPIO：TX=4，RX=5；波特率 500000
- 默认 listen-only（只听不 ACK）

### esp-can-rx

- ESP32-S3 供电即可（ESP-NOW）

## 编译烧录

需要 ESP-IDF **v5.5+**。

```bash
cd esp-can-rx && idf.py set-target esp32s3 && idf.py build flash monitor
cd esp-can-tx && idf.py set-target esp32s3 && idf.py build flash monitor
```

先运行 RX，把串口打印的 STA MAC 填到 TX 的 `EXAMPLE_ESPNOW_PEER_MAC`；两端信道默认均为 1。

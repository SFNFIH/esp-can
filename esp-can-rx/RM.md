# esp-can-rx 超级详细说明（RM）

> 本文档只描述 **接收端 / 车库端 / PC 侧** 工程 `esp-can-rx`。  
> 仓库总览请看根目录 [`README.md`](../README.md)（请勿删除）。  
> 车端发送请看 [`../esp-can-tx/RM.md`](../esp-can-tx/RM.md)。

---

## 1. 工程一句话定位

`esp-can-rx` 运行在 **ESP32-S3** 上，充当「无线 CAN 监视与回控终端」：

1. 通过 **ESP-NOW** 接收 `esp-can-tx` 转发来的车辆 CAN 帧；
2. 提供 **UART 命令行（REPL）**：查看、过滤、统计、实时打印；
3. 可选通过 CLI `can send` **回传一帧** 给 TX，由 TX 决定是否注入车辆 CAN。

它不接 CAN 收发器也能工作；适合放在电脑旁，用串口监视器操作。

---

## 2. 你需要准备什么

### 2.1 硬件

| 项目 | 说明 |
|------|------|
| MCU | ESP32-S3 开发板 |
| CAN 收发器 | **不需要**（本端不碰车辆总线） |
| USB 线 | 供电 + UART 控制台（115200） |
| 距离 | 与 TX 同信道 ESP-NOW，室内通常数米到十余米（视环境） |

### 2.2 软件环境

- ESP-IDF **v5.5+**
- 串口监视器：`idf.py monitor`（推荐，自带 REPL 交互）
- 与 TX 工程同一仓库，协议组件需保持一致

### 2.3 使用角色

| 角色 | 典型操作 |
|------|----------|
| 只看帧 | `can watch on` / `can last` |
| 过滤某 ID | `can filter 0x7E0` |
| 回传诊断请求 | `can send 0x7E0 02 10 01`（需 TX 关闭 listen-only 才上总线） |
| 绑定固定 TX | `peer AA:BB:CC:DD:EE:FF` |

---

## 3. 目录结构（本工程）

```text
esp-can-rx/
├── CMakeLists.txt
├── sdkconfig.defaults
├── bootloader_components/
│   └── README.md
├── components/
│   ├── can_espnow_proto/          # 与 TX 相同的协议头（务必同步）
│   │   ├── CMakeLists.txt
│   │   └── include/can_espnow_proto.h
│   ├── freertos_app/              # 优先级 / 事件位 / 统计周期
│   │   ├── CMakeLists.txt
│   │   └── include/freertos_app.h
│   └── can_cli/                   # UART CLI 实现
│       ├── CMakeLists.txt
│       ├── Kconfig
│       ├── can_cli.c
│       └── include/can_cli.h
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── Kconfig.projbuild
│   └── main.c                     # app_main：Wi-Fi STA + ESP-NOW + 任务
├── managed_components/            # 生成物（gitignore）
├── dependencies.lock              # 生成物（gitignore）
├── sdkconfig                      # 生成物（gitignore）
├── build/                         # 生成物（gitignore）
└── RM.md                          # 本文件
```

---

## 4. 系统架构与数据流

```text
esp-can-tx  ──ESP-NOW──►  esp_now_recv_cb（Wi-Fi 任务上下文）
                              │
                              ├── 校验 magic/version/dlc
                              ├── xQueueSend(rx_queue)
                              └── xTaskNotifyGive(can_rx)
                                      │
                                      ▼
                               can_rx 任务
                                      │
                                      ▼
                               can_cli_on_frame()
                                      ├── 写入环形缓冲（Mutex）
                                      ├── 自动学习 peer MAC + EventBit
                                      └── watch 开启时打印到 UART

反向：
  用户输入: can send ...
      │
      ▼
  CLI 组装 can_espnow_frame_t
      │ xQueueSend(tx_queue)
      ▼
  can_tx 任务
      │ esp_now_send(peer)
      ▼
  esp-can-tx inject_queue →（可选）车辆 CAN
```

### 4.1 设计要点

- ESP-NOW 接收回调 **不在 ISR**，而在 Wi-Fi 任务：因此使用 `xQueueSend` / `xTaskNotifyGive`（不是 FromISR 版本）。
- CLI **不直接** `esp_now_send`：避免 REPL 在 Wi-Fi 忙时卡住；统一交给 `can_tx`。
- 环形缓冲 + Mutex：`can last` / `watch` / 统计并发安全。

---

## 5. FreeRTOS 任务与同步原语

定义见 `components/freertos_app/include/freertos_app.h`。

### 5.1 任务表

| 任务/实体 | 优先级 | 栈 | 职责 |
|-----------|--------|-----|------|
| `can_rx` | 6 | 4096 | 等 Notify，排空 RX 队列，交给 CLI |
| Console REPL | （esp_console 内部任务） | — | 读行、解析命令 |
| `can_tx` | 4 | 4096 | 从 TX 队列取帧并 ESP-NOW 发出 |
| Soft Timer `stats` | — | — | 默认每 **5s** 打心跳日志（有流量时） |

### 5.2 队列与事件

| 对象 | 深度/位 | 用途 |
|------|---------|------|
| `s_rx_queue` | 64 | ESP-NOW → can_rx |
| `s_tx_queue` | 16 | CLI → can_tx |
| `APP_EVT_WIFI_READY` | bit0 | STA Wi-Fi 就绪 |
| `APP_EVT_ESPNOW_READY` | bit1 | ESP-NOW 就绪 |
| `APP_EVT_PEER_LEARNED` | bit2 | 已学习/已配置 TX MAC |
| `APP_EVT_ALL_READY` | WIFI+ESPNOW | can_rx 启动门槛 |

### 5.3 CLI 相关同步

| 原语 | 用途 |
|------|------|
| Mutex | 保护 ring、peer MAC、统计计数 |
| Event Group | peer 学习置位，供主程序/统计查询 |
| Queue | `can send` 入队 |

---

## 6. Wi-Fi / ESP-NOW 初始化细节

`main.c` → `wifi_espnow_init()`：

1. `esp_netif_init` + 默认事件循环  
2. Wi-Fi init，模式 **STA**（不必关联 AP）  
3. `esp_wifi_set_channel(CONFIG_EXAMPLE_ESPNOW_CHANNEL)`  
4. 打印 **本机 STA MAC**（必须抄到 TX 的 peer 配置）  
5. `esp_now_init` + 注册 recv 回调  
6. 添加 **广播 peer**（`FF:FF:FF:FF:FF:FF`），便于接收广播/未精确配对阶段的帧  
7. 置位 `APP_EVT_ESPNOW_READY`

> TX 若使用单播 MAC，RX 仍能收；RX 回传时需要知道 TX MAC（自动学习或 `peer`）。

---

## 7. UART CLI 完整手册

启动后提示符：

```text
esp-can-rx>
```

波特率：**115200**，换行建议兼容 CRLF。

### 7.1 命令总表

| 命令 | 作用 |
|------|------|
| `help` | 列出已注册命令（esp_console 内置） |
| `mac` | 打印本机 STA MAC |
| `peer` | 查看当前 TX 对端 MAC |
| `peer AA:BB:CC:DD:EE:FF` | 手动设置 TX MAC（并 add_peer） |
| `can stats` | 接收/丢弃/回传统计 + watch/filter 状态 |
| `can last [n]` | 打印最近 n 帧（默认 20，最大环大小 128） |
| `can watch on\|off` | 实时打印新帧 |
| `can clear` | 清空本地环形缓冲 |
| `can filter <id\|off>` | 只显示某 ID；`off`/`clear` 取消 |
| `can send [-e] <id> <hex...>` | 回传一帧到 TX（最多 8 字节数据） |

### 7.2 命令详解

#### `mac`

```text
esp-can-rx> mac
本机 STA MAC: AA:BB:CC:DD:EE:FF
```

把这行填进 TX：`EXAMPLE_ESPNOW_PEER_MAC`。

#### `peer`

```text
esp-can-rx> peer
对端 TX MAC: 11:22:33:44:55:66

esp-can-rx> peer 11:22:33:44:55:66
已设置对端 TX MAC: 11:22:33:44:55:66
```

未设置且尚未收到任何帧时，会提示可手动设置或等待自动学习。

#### `can watch`

```text
esp-can-rx> can watch on
已开启实时打印
t=1234 seq=10 id=0x7E0 dlc=8 - data=02 10 01 00 00 00 00 00
```

若同时开了 filter，只有匹配 ID 才打印。

#### `can filter`

```text
esp-can-rx> can filter 0x7E0
已设置过滤 ID=0x7E0

esp-can-rx> can filter off
已清除 ID 过滤
```

影响 `watch` 与 `last` 的显示过滤（缓冲里仍按接收顺序存储全量，直到 clear）。

#### `can last`

```text
esp-can-rx> can last 50
最近 50 帧（共缓冲 128）:
...
```

#### `can stats`

示例字段含义：

| 字段 | 含义 |
|------|------|
| 接收成功 | CLI 已处理的有效帧 |
| 队列丢弃 | RX 队列满导致丢帧（回调里 `can_cli_note_drop`） |
| 缓冲总数 | 写入 ring 的累计次数（可大于 128） |
| 回传成功/失败 | `can_tx` 发送结果 |
| 入队失败 | CLI `can send` 时 TX 队列满 |
| watch / filter | 当前开关状态 |

#### `can send`

```text
# 标准帧
esp-can-rx> can send 0x7E0 02 10 01

# 扩展帧（-e）
esp-can-rx> can send -e 0x18DAF110 02 10 01
```

行为：

1. 解析 ID / 可选 EXT 标志 / 数据字节；  
2. 检查已有 peer；  
3. 填 `magic/version/seq`；  
4. `xQueueSend(tx_queue)`（超时约 100ms）；  
5. 打印「已入队回传…」；实际无线发送由 `can_tx` 完成。

**重要：** TX 默认 listen-only，回传到达 TX 后 **不会** 上车辆总线，除非关闭 TX 的 listen-only。

### 7.3 自动学习 peer

`can_cli_on_frame()` 在首次收到带 `src_mac` 的帧且本地尚无 peer 时：

1. 保存 MAC，置 `s_peer_valid`  
2. `xEventGroupSetBits(PEER_LEARNED)`  
3. 若尚未存在则 `esp_now_add_peer`  
4. 日志：`Learned TX peer ...`

之后无需手动 `peer` 也能 `can send`（仍建议生产环境手动固定）。

### 7.4 Kconfig 默认 peer

`EXAMPLE_ESPNOW_PEER_MAC` 若为非全 0，启动时 CLI 会预置为对端，方便无流量时也能先发。

---

## 8. ESP-NOW 帧协议（与 TX 共用）

`components/can_espnow_proto/include/can_espnow_proto.h` **必须与 TX 字节一致**：

| 字段 | 值/含义 |
|------|---------|
| magic | `0xCA` |
| version | `0x01` |
| flags.bit0 | 扩展帧 EXT |
| flags.bit1 | 远程帧 RTR |
| dlc | 0..8 |
| id | CAN ID |
| seq | 序号 |
| data[8] | 载荷 |

接收回调丢弃：长度不对、magic/version 不符、dlc>8 → `s_rx_invalid++`。

---

## 9. menuconfig 配置项

`idf.py menuconfig` → **ESP-CAN-RX Configuration**

| 符号 | 默认 | 含义 |
|------|------|------|
| `EXAMPLE_ESPNOW_CHANNEL` | 1 | 必须与 TX 相同 |
| `EXAMPLE_ESPNOW_PEER_MAC` | 00:00:00:00:00:00 | 可选预置 TX MAC；全 0 表示不预置 |

`sdkconfig.defaults` 还打开了默认 UART console。

---

## 10. 编译、烧录、进入 CLI

```bash
cd esp-can-rx
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

看到类似：

```text
本机 STA MAC（填到 esp-can-tx 对端）: AA:BB:CC:DD:EE:FF
esp-can-rx>
```

把 MAC 配到 TX 后重新编译/烧录 TX（或改完 menuconfig 再 flash）。

退出 monitor：`Ctrl + ]`

---

## 11. 推荐联调流程

1. 先 flash **RX**，复制 STA MAC。  
2. TX menuconfig 写入该 MAC，信道确认同为 1。  
3. flash **TX**，接上 CAN（或台架信号）。  
4. RX 执行：

```text
can watch on
can stats
can last 20
```

5. 需要回传时：

```text
peer          # 确认已学习
can send 0x7E0 02 10 01
```

并确认 TX 已关闭 listen-only（若要真上总线）。

---

## 12. 故障排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| 完全收不到帧 | 信道/MAC/距离 | 对齐信道；TX 暂用广播试；拉近 |
| `invalid` 涨 | 协议不一致 | 对比两端 `can_espnow_proto.h` |
| `q_drop` 涨 | 总线太快 / CLI 太忙 | 提高处理；开 filter；减小 TX 日志 |
| `can send` 提示无 peer | 尚未学习 | `peer AA:BB:...` 或先收几帧 |
| 回传 TX 有日志但不进车 | TX listen-only | 关闭 TX listen-only |
| REPL 乱码/无提示符 | 波特率/USB 口 | 115200；正确端口；重插 USB |
| 统计心跳太吵 | Soft Timer 5s | 正常；无流量时不打印 |

---

## 13. 代码阅读地图

| 想了解… | 先看 |
|---------|------|
| 启动与任务 | `main/main.c` |
| 收包校验与 Notify | `main/main.c` → `espnow_recv_cb` / `can_rx_task` |
| 回传发送 | `main/main.c` → `can_tx_task` |
| 全部 CLI 命令 | `components/can_cli/can_cli.c` |
| CLI 对外 API | `components/can_cli/include/can_cli.h` |
| 协议 | `components/can_espnow_proto/include/can_espnow_proto.h` |
| 优先级 | `components/freertos_app/include/freertos_app.h` |

### 13.1 `can_cli` API（供 main 调用）

```c
esp_err_t can_cli_start(EventGroupHandle_t events, QueueHandle_t tx_queue);
void can_cli_on_frame(const can_espnow_frame_t *frame, const uint8_t src_mac[6]);
void can_cli_note_drop(void);
void can_cli_note_tx_result(bool ok);
bool can_cli_get_peer(uint8_t mac_out[6]);
```

---

## 14. 与 esp-can-tx 的契约

1. 协议头一致。  
2. 信道一致。  
3. TX 配置 RX 的 STA MAC（或广播联调）。  
4. RX 学习/配置 TX MAC 后才能回传。  
5. 回传上总线依赖 TX 关闭 listen-only。  
6. SoftAP 网页是 TX 的功能；RX **没有**网页服务。

---

## 15. 性能与限制

| 项目 | 现状 |
|------|------|
| RX 队列 | 64 帧；满则丢并计数 |
| TX 队列 | 16 帧；CLI 入队失败会提示 |
| 本地 ring | 128 帧；溢出覆盖最旧逻辑取决于实现（按累计 index 取模） |
| 控制台 | 单行编辑，适合人工操作，不适合当高速 logger |
| ESP-NOW | 无应用层加密（peer.encrypt=false） |

高速总线（接近满载 500k）时：优先在 TX 侧过滤，或 RX 用 filter + 关闭 watch，改用周期性 `can last`。

---

## 16. 安全提示

- 回传注入可能影响实车，仅在授权台架使用。  
- 无线明文：实验室可用；公开环境需自行加加密/认证。  
- 不要把含实车完整日志的监控输出随意上传。

---

## 17. 快速命令备忘

```bash
cd esp-can-rx
idf.py set-target esp32s3
idf.py menuconfig
idf.py build flash monitor
```

进 CLI 后最小闭环：

```text
mac
can watch on
can stats
can send 0x123 01 02 03
```

---

## 18. 维护建议

- 改协议：同步修改 TX/RX 的 `can_espnow_proto`，并提升 version。  
- 加新 CLI 子命令：在 `can_cli.c` 的 `cmd_can` 分发，并更新本 RM 第 7 节。  
- 若要把 CLI 优先级固定：可评估替换 esp_console 默认任务参数（进阶）。  
- 需要文件日志：可另加 SD/USB 组件，不要在 `espnow_recv_cb` 里做重 IO。

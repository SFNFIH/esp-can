# esp-can-tx 超级详细说明（RM）

> 本文档只描述 **车端 / 发送端** 工程 `esp-can-tx`。  
> 仓库总览请看根目录 [`README.md`](../README.md)（请勿删除）。  
> 接收端请看 [`../esp-can-rx/RM.md`](../esp-can-rx/RM.md)。

---

## 1. 工程一句话定位

`esp-can-tx` 运行在 **ESP32-S3** 上，充当「汽车 CAN 网关」：

1. 通过 **TWAI（CAN）** 监听车辆总线帧；
2. 用 **ESP-NOW** 把帧无线转发给 `esp-can-rx`；
3. 开启 **SoftAP 热点 + HTTP 网页**，手机/电脑可实时看帧；
4. 可选接收 `esp-can-rx` 回传的帧，并 **注入回车辆 CAN**（需关闭 listen-only）。

它是整条链路里「最靠近汽车」的一端，实时性要求最高。

---

## 2. 你需要准备什么

### 2.1 硬件

| 项目 | 说明 |
|------|------|
| MCU | ESP32-S3 开发板（建议带 USB-UART） |
| CAN 收发器 | 如 SN65HVD230 / TJA1050 等，3.3V 逻辑电平 |
| 连线 | ESP32 TX GPIO → 收发器 TXD；ESP32 RX GPIO → 收发器 RXD；收发器 CANH/CANL 并入车辆总线 |
| 供电 | 开发板 5V/USB；收发器按芯片手册供电 |
| 终端电阻 | 若你是临时并联嗅探，通常 **不要再加 120Ω**（车上已有）；若是独立实验台总线，两端各 120Ω |

默认引脚（可在 `menuconfig` 改）：

- TWAI TX = **GPIO4**
- TWAI RX = **GPIO5**
- 波特率 = **500000**（很多乘用车常用；柴油/商用车也可能是 250000）

### 2.2 软件环境

- ESP-IDF **v5.5+**（本工程使用新版 `esp_driver_twai` / `esp_twai.h` API）
- Python3、CMake、Ninja（IDF 安装器会带）
- 串口工具：`idf.py monitor` 或任意 115200 终端

### 2.3 安全与法律（必读）

- 默认开启 **listen-only**：只听不 ACK，尽量降低对车辆总线的干扰。
- 即使 listen-only，并联车辆总线仍有风险（接线错误、波特率错误、短路等）。
- **仅用于合法授权的学习/诊断场景**；上路车辆改装与法规因地区而异，自行负责。
- 若关闭 listen-only 并向总线注入帧，错误帧可能导致 ECU 异常，务必先在台架验证。

---

## 3. 目录结构（本工程）

```text
esp-can-tx/
├── CMakeLists.txt                 # 工程入口：project(esp-can-tx)
├── sdkconfig.defaults             # 默认目标芯片与示例配置
├── bootloader_components/         # 预留：自定义 bootloader 组件（当前为空说明）
│   └── README.md
├── components/                    # 本地组件（IDF 自动扫描）
│   ├── can_espnow_proto/          # CAN-over-ESP-NOW 协议头（与 RX 保持一致）
│   │   ├── CMakeLists.txt
│   │   └── include/can_espnow_proto.h
│   ├── freertos_app/              # 任务优先级、栈、事件位常量
│   │   ├── CMakeLists.txt
│   │   └── include/freertos_app.h
│   └── web_monitor/               # SoftAP HTTP 网页监视器
│       ├── CMakeLists.txt
│       ├── Kconfig
│       ├── web_monitor.c
│       └── include/web_monitor.h
├── main/
│   ├── CMakeLists.txt             # 注册 main.c 及其依赖组件
│   ├── idf_component.yml          # 组件管理器清单（要求 IDF >= 5.5）
│   ├── Kconfig.projbuild          # menuconfig 里的「ESP-CAN-TX Configuration」
│   └── main.c                     # app_main：TWAI + ESP-NOW + 任务管线
├── managed_components/            # 编译时由组件管理器生成（gitignore）
├── dependencies.lock              # 依赖锁定文件（gitignore）
├── sdkconfig                      # 本地完整配置（gitignore）
├── build/                         # 编译产物（gitignore）
└── RM.md                          # 本文件
```

根目录的 [`README.md`](../README.md) 是总览；**本文件是 TX 的完整手册**。

---

## 4. 系统架构与数据流

```text
车辆 CAN 总线
      │  (CANH/CANL)
      ▼
  CAN 收发器
      │  TXD/RXD
      ▼
 ESP32-S3 TWAI 控制器
      │
      │  RX ISR：写入帧池 + TaskNotify
      ▼
  can_rx 任务（最高优先级之一）
      │
      ├── Queue ──► espnow_tx 任务 ──► ESP-NOW ──► esp-can-rx
      │
      └── Queue ──► web_feed 任务 ──► web_monitor 环形缓冲
                                        │
                                        ▼
                              SoftAP HTTP :80
                              http://192.168.4.1/

反向路径（可选注入）：
  esp-can-rx CLI "can send"
      │ ESP-NOW
      ▼
  espnow_recv_cb ──► inject_queue ──► can_inject 任务
      │
      ▼
  twai_node_transmit（仅 listen-only=OFF 时真正上总线）
```

### 4.1 为什么要这样拆任务？

| 路径 | 风险 | 本工程做法 |
|------|------|------------|
| TWAI ISR 里做太多事 | ISR 过长丢帧 | ISR 只填池 + Notify |
| ESP-NOW 发送阻塞 | 拖慢 CAN 接收 | 独立 `espnow_tx` 任务 |
| HTTP/网页慢 | 拖慢无线转发 | 独立 `web_feed` 任务 + 环形缓冲 |
| 统计刷新 | 占一个死循环 delay 任务 | Soft Timer 1Hz |

---

## 5. FreeRTOS 任务与同步原语

定义见 `components/freertos_app/include/freertos_app.h`。

### 5.1 任务表

| 任务名 | 优先级 | 栈 | 职责 |
|--------|--------|-----|------|
| `can_rx` | 6 | 4096 | 等 TaskNotify，排空 TWAI 帧池，扇出到两个 Queue |
| `espnow_tx` | 5 | 4096 | 从 ESP-NOW 队列取帧并 `esp_now_send` |
| `web_feed` | 4 | 3072 | 从 Web 队列取帧并写入网页环形缓冲 |
| `can_inject` | 4 | 4096 | 从 inject 队列取回传帧，尝试写入 TWAI |
| Soft Timer `stats` | — | — | 每 1s 把计数推给网页 |

### 5.2 同步原语

| 原语 | 用途 |
|------|------|
| Counting Semaphore `free_pool_sem` | TWAI 帧池空闲槽位数 |
| Task Notification | ISR → `can_rx` 唤醒（计数 = 待处理帧数） |
| Queue `espnow_queue` / `web_queue` | 扇出，互不阻塞 |
| Queue `inject_queue` | ESP-NOW 回传注入 |
| Binary Semaphore `espnow_lock` | 串行化 send 与 send_cb |
| Event Group | WIFI / ESPNOW / TWAI / PEER 就绪位 |
| Soft Timer | 1Hz 统计 |

### 5.3 Event Group 位

| 位 | 含义 |
|----|------|
| `APP_EVT_WIFI_READY` | SoftAP/STA Wi-Fi 已启动 |
| `APP_EVT_ESPNOW_READY` | ESP-NOW 初始化完成 |
| `APP_EVT_TWAI_READY` | TWAI 节点已 enable |
| `APP_EVT_PEER_CONFIGURED` | 对端 MAC 已从 Kconfig 解析成功 |
| `APP_EVT_ALL_READY` | WIFI+ESPNOW+TWAI（业务任务等待此组合） |

---

## 6. TWAI（CAN）细节

### 6.1 初始化要点（`main.c`）

- 使用 on-chip TWAI node：`twai_new_node_onchip`
- 开放掩码过滤器（收所有标准帧过滤槽；扩展帧同样可进，取决于驱动与配置）
- 注册回调：
  - `on_rx_done`：ISR 收帧
  - `on_error`：错误日志
  - `on_state_change`：状态迁移日志
- `CONFIG_EXAMPLE_TWAI_LISTEN_ONLY=y` 时置 `enable_listen_only`

### 6.2 ISR 收帧流程

1. `TakeFromISR(free_pool_sem)`，失败则 `s_drop_count++`
2. `twai_node_receive_from_isr` 填入当前 `write_idx` 槽
3. `write_idx = (write_idx+1) % depth`
4. `s_rx_count++`
5. `vTaskNotifyGiveFromISR(can_rx_task)`

### 6.3 `can_rx` 消费流程

1. `ulTaskNotifyTake` 得到待处理计数 `n`
2. 循环 `n` 次：读 `read_idx` 槽 → 组装 `can_queued_frame_t`（含递增 `seq`）
3. `Give(free_pool_sem)` 归还槽
4. 非阻塞投递 `espnow_queue` 与 `web_queue`（满则 drop）

### 6.4 listen-only 与注入

| 模式 | 行为 |
|------|------|
| listen-only **开**（默认） | 只听不 ACK；`can_inject` 收到回传会打日志并计 fail，**不上总线** |
| listen-only **关** | 可 ACK；`can_inject` 调用 `twai_node_transmit` 真正发到 CAN |

关闭方法：

```bash
idf.py menuconfig
# ESP-CAN-TX Configuration → Listen-only mode on vehicle CAN bus → 取消勾选
```

或改 `sdkconfig.defaults` / `sdkconfig` 中 `CONFIG_EXAMPLE_TWAI_LISTEN_ONLY`。

---

## 7. Wi-Fi SoftAP + ESP-NOW

### 7.1 模式

- `WIFI_MODE_APSTA`：  
  - **AP**：给手机/电脑连网页  
  - **STA 接口**：承载 ESP-NOW（不连路由器也行）

### 7.2 默认热点

| 项 | 默认值 |
|----|--------|
| SSID | `ESP-CAN-TX` |
| 密码 | `espcan123`（≥8 字符 → WPA2） |
| 信道 | `1`（与 ESP-NOW 共用） |
| 网关/页面 | `http://192.168.4.1/` |
| 最大连接 | 4 |

> SoftAP **没有外网**。页面是本地静态 HTML，不要指望 CDN 字体/外链资源。

### 7.3 ESP-NOW 对端

- Kconfig：`EXAMPLE_ESPNOW_PEER_MAC`
- 默认 `FF:FF:FF:FF:FF:FF`（广播，便于初次联调）
- **正式使用请填 esp-can-rx 的 STA MAC**（RX 启动日志会打印）

两端 **信道必须一致**（默认都是 1）。

### 7.4 发送串行化

1. `espnow_tx` 任务 `Take(espnow_lock, 50ms)`
2. `esp_now_send(...)`
3. 成功路径：等 `espnow_send_cb` 里 `Give(espnow_lock)` 并更新 ok/fail
4. 调用失败：本任务立刻 `Give` 并计 fail

---

## 8. 网页监视器（web_monitor）

### 8.1 访问步骤

1. 烧录运行 TX
2. 手机/电脑连接 Wi-Fi：`ESP-CAN-TX` / `espcan123`
3. 浏览器打开：**http://192.168.4.1/**
4. 看到「ESP-CAN 监视器」页面；有帧时表格滚动更新

### 8.2 HTTP 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 返回内嵌 HTML/CSS/JS 单页 |
| GET | `/api/frames?since=<n>` | JSON：统计 + 自 `since` 起的新帧 |

页面每 **200ms** 轮询一次 `/api/frames`（非 WebSocket，避免 SoftAP 下复杂生命周期问题）。

### 8.3 页面功能

- 统计卡片：接收 / 转发成功 / 转发失败 / 丢弃 / 页面帧每秒
- 连接状态：已连接 / 断开重试
- 暂停 / 清空
- 「只看扩展帧」过滤
- 表格列：时间ms、序号、ID、DLC、标志、数据

### 8.4 内部缓冲

- 环形缓冲约 128 帧
- Mutex 保护读写
- `web_feed` 只负责写入；HTTP handler 负责读取拼 JSON

---

## 9. ESP-NOW 帧协议（与 RX 共用）

文件：`components/can_espnow_proto/include/can_espnow_proto.h`  
**必须与 esp-can-rx 中同名组件保持字节级一致。**

```text
偏移  字段       大小    说明
0     magic      1       固定 0xCA
1     version    1       固定 0x01
2     flags      1       bit0=EXT, bit1=RTR
3     dlc        1       0..8
4     id         4       CAN ID（小端，packed struct）
8     seq        4       TX 侧递增序号
12    data[8]    8       有效字节由 dlc 决定
合计 20 字节（packed）
```

校验：`magic==0xCA && version==0x01 && dlc<=8`。

---

## 10. menuconfig 配置项全表

路径：`idf.py menuconfig` → **ESP-CAN-TX Configuration**

| Kconfig 符号 | 默认 | 含义 |
|--------------|------|------|
| `EXAMPLE_TWAI_TX_GPIO` | 4 | TWAI TX 引脚 |
| `EXAMPLE_TWAI_RX_GPIO` | 5 | TWAI RX 引脚 |
| `EXAMPLE_TWAI_BITRATE` | 500000 | 总线波特率 |
| `EXAMPLE_TWAI_LISTEN_ONLY` | y | 只听不 ACK |
| `EXAMPLE_WIFI_AP_SSID` | ESP-CAN-TX | 热点名 |
| `EXAMPLE_WIFI_AP_PASSWORD` | `espcan123` | 热点密码（≥8 字符启用 WPA2） |
| `EXAMPLE_ESPNOW_CHANNEL` | 1 | Wi-Fi/ESP-NOW 信道 |
| `EXAMPLE_ESPNOW_PEER_MAC` | FF:FF:FF:FF:FF:FF | RX 的 STA MAC |
| `EXAMPLE_CAN_QUEUE_LEN` | 64 | 帧池/转发队列深度 |
| `EXAMPLE_LOG_EVERY_N_FRAMES` | 50 | 每 N 帧打一条 UART 日志（0=每帧） |

`sdkconfig.defaults` 已写入常用默认值；首次 `set-target` / `build` 会生成 `sdkconfig`。

---

## 11. 编译、烧录、监视

在仓库中进入本工程目录：

```bash
cd esp-can-tx
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
# Windows 可能是 COM3 等；可用 idf.py flash monitor 让工具自动选口
```

退出 monitor：`Ctrl + ]`

常见编译问题：

- IDF 版本 < 5.5：TWAI 新 API 不存在 → 升级 IDF
- 找不到组件：确认在 `esp-can-tx/` 目录执行，而不是仓库根目录
- 改了 Kconfig 不生效：`idf.py fullclean && idf.py build`

---

## 12. 联调步骤（推荐顺序）

1. **只起 RX**：看串口打印的 STA MAC，记下来。  
2. **配置 TX 对端 MAC**：menuconfig 填入 RX MAC（不要一直用广播）。  
3. **确认信道一致**（默认 1）。  
4. **起 TX**：连 SoftAP，打开网页；UART 应看到转发日志。  
5. **RX**：`can watch on`，应看到帧。  
6. （可选）TX 关闭 listen-only；RX `can send ...` 验证注入。

台架验证建议：用 USB-CAN 或第二台 MCU 制造已知 ID 帧，再对照网页/CLI。

---

## 13. 日志与计数含义

| 计数 | 含义 |
|------|------|
| `rx` / `s_rx_count` | TWAI ISR 成功入池的帧数 |
| `fwd_ok` | ESP-NOW 发送回调成功 |
| `fwd_fail` | 发送失败或超时拿不到锁 |
| `drop` | 帧池满、扇出队列满等丢弃 |
| `inject_ok/fail/drop` | 回传注入成功/失败/队列满 |

网页统计由 Soft Timer 每秒刷新；UART 按 `LOG_EVERY_N` 抽样打印。

---

## 14. 故障排查清单

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| 网页打不开 | 没连上 SoftAP / IP 不对 | 连 `ESP-CAN-TX`，访问 192.168.4.1 |
| 网页「连接断开」 | HTTP 服务未起 / 浏览器缓存 | 看 TX 日志是否 `Web monitor ready`；强刷 |
| 有网页无统计但 RX 无帧 | 对端 MAC/信道不对 | 核对 RX STA MAC 与信道 |
| RX 有帧但网页空 | web 队列满或 feed 卡住 | 看 drop 计数；降总线负载试 |
| 总线一接就 error | 波特率/极性/短路 | 确认 500k/250k；查 TX/RX 是否接反 |
| 注入无效 | listen-only 仍开启 | 关闭 `EXAMPLE_TWAI_LISTEN_ONLY` |
| ESP-NOW 大量 fail | 距离/干扰/未加 peer | 拉近距离；确认 `add_peer` 成功 |

---

## 15. 代码阅读地图

| 想了解… | 先看 |
|---------|------|
| 启动顺序、任务创建 | `main/main.c` → `app_main` |
| TWAI ISR/池 | `main/main.c` → `twai_on_rx_cb` / `can_rx_task` |
| SoftAP + ESP-NOW | `main/main.c` → `wifi_ap_espnow_init` |
| 网页 HTML/API | `components/web_monitor/web_monitor.c` |
| 协议字段 | `components/can_espnow_proto/include/can_espnow_proto.h` |
| 优先级常量 | `components/freertos_app/include/freertos_app.h` |
| 可配置项 | `main/Kconfig.projbuild` |

---

## 16. 与 esp-can-rx 的契约

1. 协议头 **完全一致**（magic/version/flags/布局）。  
2. Wi-Fi 信道一致。  
3. TX 知道 RX 的 STA MAC（或临时广播）。  
4. RX 可通过学习或手动 `peer` 知道 TX MAC 才能回传。  
5. 回传要上车辆总线 ⇒ TX listen-only 必须关闭。

---

## 17. 版本与维护建议

- 改协议字段时：同时改 TX/RX 两个 `can_espnow_proto`，并升 `CAN_ESPNOW_VERSION`。  
- 改网页：只改 `web_monitor.c` 内嵌字符串；注意 SoftAP 无外网。  
- 提速：优先保证 `can_rx` 不被打印/HTTP 拖住；日志用 `LOG_EVERY_N`。  
- 量产：关闭广播 peer、固定 MAC、评估加密 ESP-NOW（需改协议与密钥管理）。

---

## 18. 快速命令备忘

```bash
cd esp-can-tx
idf.py set-target esp32s3
idf.py menuconfig
idf.py build flash monitor
```

手机连热点 → 浏览器打开：

```text
http://192.168.4.1/
```

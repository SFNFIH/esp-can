# Agent 交接备忘（esp-can）

供后续 Cursor Agent 快速接手。仓库：https://github.com/SFNFIH/esp-can  
默认分支：`main`（交接撰写时 tip=`d2677b8`；接手先 `git pull`）。

## 项目是什么

ESP-IDF（文档要求 **v5.5+**）双工程，目标芯片 **esp32s3**，**TWAI(CAN) + ESP-NOW**：

| 工程 | 路径 | 作用 |
|------|------|------|
| TX | [`esp-can-tx/`](esp-can-tx/) | 接汽车 CAN/OBD；ESP-NOW 转发；SoftAP 网页监视；可接收 RX 回传并注入 CAN |
| RX | [`esp-can-rx/`](esp-can-rx/) | 收 ESP-NOW；UART CLI 查看/过滤/`can send` 回传 |

手册：根 [`README.md`](README.md)、[`esp-can-tx/RM.md`](esp-can-tx/RM.md)、[`esp-can-rx/RM.md`](esp-can-rx/RM.md)。

共享帧协议（两端须字节级一致）：

- `esp-can-tx/components/can_espnow_proto/include/can_espnow_proto.h`
- `esp-can-rx/components/can_espnow_proto/include/can_espnow_proto.h`

TX 默认硬件（见 `sdkconfig.defaults` / Kconfig）：CAN TX=GPIO4、RX=GPIO5，500 kbps，**`CONFIG_EXAMPLE_TWAI_LISTEN_ONLY=y`**（只听不 ACK）。要对总线注入须关闭 listen-only。  
SoftAP 默认：SSID `ESP-CAN-TX` / 密码 `espcan123` → http://192.168.4.1/  
ESP-NOW 对端：Kconfig `EXAMPLE_ESPNOW_PEER_MAC`（先跑 RX 看 STA MAC 再填 TX）。

## 本会话已完成（已合入 main）

- **PR #1 MERGED**：https://github.com/SFNFIH/esp-can/pull/1  
- **用户确认效果正确**，并要求合并主分支（已完成）。
- 功能：SoftAP 网页监视改为 **每个 CAN ID 固定一行**，同 ID 新帧 **原地刷新**；按 ID 排序；变化短暂高亮；唯一 ID 软上限 **256**。
- 主要改动文件：
  - [`esp-can-tx/components/web_monitor/web_monitor.c`](esp-can-tx/components/web_monitor/web_monitor.c)（内嵌 HTML/JS）
  - [`esp-can-tx/RM.md`](esp-can-tx/RM.md) 网页监视说明
- **未改**：C 侧环形缓冲与 `GET /api/frames?since=` 语义（仍按帧流推送；聚合在浏览器完成）。

### 网页逻辑（勿回退成流水日志）

- 行键：`(flags&1 ? 'e' : 's') + ':' + id`
- `Map` 聚合 + 按数值 ID 插入排序
- 「只看扩展帧」：不匹配行加 `hidden`（`display:none`），仍 upsert
- 列：`ID | DLC | 标志 | 数据 | 序号 | 时间ms`
- SoftAP 页：http://192.168.4.1/（SSID/密码见 `EXAMPLE_WIFI_AP_SSID` / `EXAMPLE_WIFI_AP_PASSWORD`）

Cloud 会话里曾用抽出 HTML + mock `/api/frames` 做浏览器验证；**未在实车/实板上 flash**。

## 用户明确需求 / 未完成

1. **希望 Agent 自己用 `idf.py` 做调试闭环**（`build` / `flash` / `monitor`），而不是只分析粘贴的 log。  
2. 目标场景：本机有 ESP-IDF，板子接到 **汽车 OBD/CAN**。  
3. **发起该需求时的 Cloud Agent VM 做不到实机调试**：无 `IDF_PATH` / 无 `idf.py`、无串口设备、无已连接 Self-hosted Worker。  
4. 已向用户说明：要用 **Cursor 本地 Agent**，或在接 OBD 的机器上启动 **`cursor worker start`（Self-hosted）**，再开新会话。

### 有串口时的推荐命令

```bash
# 建议先 RX：把 monitor 打印的 STA MAC 配进 TX 的 EXAMPLE_ESPNOW_PEER_MAC；信道与 SoftAP 一致（默认见 Kconfig）
cd esp-can-rx && idf.py set-target esp32s3 && idf.py build flash monitor
cd esp-can-tx && idf.py set-target esp32s3 && idf.py build flash monitor
```

RX CLI（见 `esp-can-rx/RM.md`）：

```text
can watch on
can filter 0x7E0
can send 0x7E0 02 10 01
```

注入上总线前：TX 关闭 `EXAMPLE_TWAI_LISTEN_ONLY`，并评估实车风险。

### 关键代码入口

| 区域 | 路径 |
|------|------|
| TX 主任务 / TWAI / ESP-NOW / 注入 | `esp-can-tx/main/main.c` |
| 网页监视 | `esp-can-tx/components/web_monitor/web_monitor.c` |
| RX 主任务 | `esp-can-rx/main/main.c` |
| RX CLI | `esp-can-rx/components/can_cli/can_cli.c` |
| TX 默认 sdkconfig | `esp-can-tx/sdkconfig.defaults` |

## Cloud / 协作约定（本任务批次）

- 功能分支命名：`cursor/<descriptive-name>-7c20`
- 开/更新 PR：用 **ManagePullRequest**；`gh` 只读
- 用户曾明确要求「合并到主分支」时，用 git merge + push `main`（PR #1 已显示 MERGED）
- 仓库内当时无 committed `.cursor/environment.json`（个人环境为 dashboard 侧）；`environment-info` 曾报 `no_finished_builds`

## 给下一任 Agent 的建议第一步

1. `git checkout main && git pull`，确认网页固定 ID 改动在树上。  
2. 检测环境：`command -v idf.py`、`echo $IDF_PATH`、`ls /dev/ttyUSB* /dev/ttyACM*`（或 macOS 对应串口）。  
3. **若无 idf/串口**：不要假装已 flash；提醒用户改用 Local Agent 或 Self-hosted Worker。  
4. **若有**：按用户目标（只监视 / 找 ID / UDS 回传）直接 `idf.py build flash monitor`，用 TWAI / ESP-NOW / 网页计数与 log 收窄问题。  
5. 读本文件 + 对应 `RM.md`，避免重复已否决的网页流水模式。

## 不要做的事

- 不要把网页改回「每帧 prepend 一行」的滚动日志（用户已认可固定 ID）。  
- 不要在未关 listen-only、未评估风险时对实车默认大量注入。  
- 不要假设 Cloud VM 能访问用户车上的 OBD 串口。

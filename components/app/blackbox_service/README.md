# blackbox_service

应用层黑匣子服务。该组件在 `middleware/blackbox` 通用循环存储之上定义版本化状态快照，
并统一管理 ESP_LOG 自动采集、关键事件记录、周期快照和 NVS 配置。

## 模块特点

- **应用层策略集中管理**：middleware 只负责通用异步落盘，本组件决定何时保存快照和事件
- **版本化快照协议**：`SnapshotV1` 首字节为版本号，字段变化时新增版本以保持历史日志可解析
- **ESP_LOG 自动采集**：普通 `WARN` / `ERROR` 保存文本，并且每一条都强制追加状态快照
- **稳定事件标记**：业务组件通过 `diagnostic_log` 标记关键 `INFO`，无需依赖黑匣子接口
- **轻量日志钩子**：vprintf Hook 使用固定捕获槽位和 RAM 事件环，后台任务负责向 Flash 队列提交记录
- **丢失可观测**：捕获槽位不足、事件环已满或格式化失败会累计计数，由后台任务写入汇总事件
- **周期快照**：后台任务按 NVS 配置周期采样，间隔为 `0` 时关闭
- **结构化快照限流**：默认限制相邻快照至少间隔 `100ms`，关键事件可强制记录
- **关键状态接口**：`append_event()` 优先保存事件文本，再尝试追加状态快照
- **多行诊断接口**：`append_text_event()` 仅写文本，不重复追加快照，适合启动参数块

## 文件职责

| 文件 | 职责 |
|------|------|
| `src/blackbox_service.cpp` | 服务初始化、公开事件接口和公开配置接口 |
| `src/blackbox_snapshot.cpp` | `SnapshotV1` 状态采样与结构化记录写入 |
| `src/blackbox_config.cpp` | `bb_snap_s` NVS 配置和运行期线程安全访问 |
| `src/blackbox_log_capture.cpp` | ESP_LOG 钩子、Log V1 解析和固定 RAM 事件环 |
| `src/blackbox_worker.cpp` | 自动日志事件消费和周期快照任务 |
| `private_include/blackbox_service_internal.h` | 组件内部接口，不向其他组件暴露 |

## 数据流

```mermaid
flowchart LR
    Log["ESP_LOG / DEVICE_*"] --> Hook["vprintf Hook<br/>解析稳定标记"]
    Hook --> Ring["固定 RAM 事件环"]
    Ring --> Worker["blackbox_service 后台任务"]
    Timer["周期配置 bb_snap_s"] --> Worker
    Boot["启动诊断<br/>直接同步接口"] --> Snapshot
    Worker --> Snapshot["采样 get_global_state()"]
    Snapshot --> Payload["SnapshotV1"]
    Payload --> MW["middleware/blackbox<br/>append_typed()"]
    MW --> Flash["circular_flash_buffer"]
```

## SnapshotV1

| 字段 | 类型 | 说明 |
|------|------|------|
| `version` | `uint8_t` | 快照版本，当前为 1 |
| `flags` | `GlobalStateFlags` | 诊断状态位 |
| `protect_states` | `protect_states_t` | 四路保护状态 |
| `voltage_mV` / `current_uA` | 整数 | 电压与电流 |
| `meter_mwh` | `float` | LP Core 开机以来累计能量，单位 mWh |
| `board_temperature` / `chip_temperature` | `int16_t` | 温度，单位 0.01°C |

不要直接持久化 `GlobalState`。字段语义变化时新增快照版本，并在读取端分版本解析。

## ESP_LOG 规则

| 日志形式 | 黑匣子行为 |
|----------|------------|
| 普通 `ESP_LOGI` | 忽略，不写文本也不写快照 |
| `DEVICE_EVENT_I` | 保存 `[I][TAG] 正文`，不追加快照 |
| `DEVICE_STATE_I` | 保存 `[I][TAG] 正文`，并强制追加快照 |
| 普通或标记的 `ESP_LOGW` / `ESP_LOGE` | 保存文本，并且每一条都无条件强制追加快照 |
| `DEBUG` / `VERBOSE` | 忽略 |

为避免 Flash 错误形成反馈循环，自动采集只排除 `Blackbox`、`BlackBox` 和
`CircularFlashBuffer` TAG。其余组件和 ESP-IDF 驱动的每一条 `WARN` / `ERROR` 都会
保存文本并强制追加快照。当前解析逻辑基于工程启用的 ESP-IDF Log V1；切换到 Log V2
时需要同步调整。

Hook 只接受消息正文开头的 `@DLOG1:T@` / `@DLOG1:S@` 版本化标记，并在串口输出前
移除标记。不会通过 TAG 或正文关键词猜测关键事件。业务代码应使用
[`diagnostic_log`](https://github.com/qingmeijiupiao/wireless-power-components/blob/86e167a755eec29b38ce2f469fb3b6ba6a4fd0bb/components/common/diagnostic_log/README.md) 宏，不要手写标记。

捕获使用 4 个固定槽位和 32 项 RAM 环；拥堵时不阻塞业务日志输出。丢失计数由 Worker
汇总为 `capture_drop` 事件，避免在 Hook 内打印日志形成递归。

## 集成与使用

```cpp
#include "blackbox_service.h"
#include "diagnostic_log.h"

// Blackbox::init() 和 NVS 初始化完成后尽早调用
BlackboxService::init();

// 写入一条结构化快照
BlackboxService::append_snapshot();

// 关键状态变化忽略 100ms 最小间隔
BlackboxService::append_snapshot(true);

// 普通业务关键事件只依赖 diagnostic_log
DEVICE_EVENT_I(TAG, "config: changed source=web value=%u", value);
DEVICE_STATE_I(TAG, "output: state old=0 new=1 source=web result=ok");

// 设置周期快照，单位秒；0 表示关闭
ESP_ERROR_CHECK(BlackboxService::set_snapshot_interval_s(10, "ShellCommand"));
```

## API

| API | 说明 |
|-----|------|
| `init()` | 恢复 NVS 配置、创建后台任务并安装 ESP_LOG 捕获钩子 |
| `append_snapshot(force=false)` | 采样当前 `GlobalState` 并写入 `STRUCTURED` 记录；默认受 `100ms` 最小间隔限制 |
| `append_event(fmt, ...)` | 保存关键状态变化或故障文本，再尝试追加全局快照 |
| `append_text_event(fmt, ...)` | 仅保存文本事件，不追加快照；保留给启动诊断、黑匣子管理和服务内部记录 |
| `get_snapshot_interval_s()` | 获取周期快照间隔，`0` 表示关闭 |
| `set_snapshot_interval_s(seconds, source)` | 设置周期快照间隔并持久化，返回 NVS 写入结果；调用方传入自身静态 TAG |

## 日志约定

- `SnapshotV1` 二进制布局保持不变，时间使用记录头已有的毫秒时间戳。
- 普通业务组件使用 `DEVICE_EVENT_I` / `DEVICE_STATE_I`，不直接依赖 `blackbox_service`。
- `ESP_LOGW` / `ESP_LOGE` 已由 Hook 自动持久化，禁止再追加相同黑匣子文本。
- 直接 `append_event()` / `append_text_event()` 仅保留给启动诊断、黑匣子管理和结构化快照边界。
- 启动基础诊断块使用直接接口分行记录并逐行同步落盘，保证重启早期路径完整。
- 调用来源由业务组件传入自身编译期 `TAG` 或局部静态字符串，黑匣子组件不维护来源枚举。
- 允许记录 SSID、IP 和 MAC；禁止记录 WiFi 密码和 HTTP 请求体。

## NVS Key

| Key | 类型 | 默认值 | 说明 |
|-----|------|-------:|------|
| `bb_snap_s` | blob(uint32_t) | `0` | 周期快照间隔，单位秒；`0` 表示关闭 |

## 环境与依赖

| 类别 | 要求 |
|------|------|
| 框架 | ESP-IDF v6.0+ |

<!-- dependency-links:start -->
## 依赖导航

工程内直接依赖：

- [`global_state`](../global_state/README.md)（`app`）
- [`blackbox`](https://github.com/qingmeijiupiao/wireless-power-components/blob/86e167a755eec29b38ce2f469fb3b6ba6a4fd0bb/components/middleware/blackbox/README.md)（`middleware`）
- [`HXC_NVS`](https://github.com/qingmeijiupiao/wireless-power-components/blob/86e167a755eec29b38ce2f469fb3b6ba6a4fd0bb/components/bsp/HXC_NVS/README.md)（`bsp`）
- [`diagnostic_log`](https://github.com/qingmeijiupiao/wireless-power-components/blob/86e167a755eec29b38ce2f469fb3b6ba6a4fd0bb/components/common/diagnostic_log/README.md)（`common`）

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->

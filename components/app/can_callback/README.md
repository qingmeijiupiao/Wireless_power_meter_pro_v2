# can_callback

`can_callback` 是设备 CAN 应用协议的入口。它初始化 CAN 终端电阻和 TWAI 驱动，注册设备支持的命令，并启动一个诊断任务记录总线异常。

初次阅读时可以先记住：`HXC_TWAI` 负责收发和按 ID 分发，`can_callback` 负责“收到某条命令后做什么”。

## 设计目标

- 集中注册设备 CAN 命令，避免业务回调散落在不同模块。
- 使用 NVS 保存设备 ID 和波特率。
- 将输出控制、终端电阻控制和状态查询连接到对应业务组件。
- 默认启用**硬件验收过滤器**，只接收本机控制帧（`CAN_ID .. CAN_ID+3`），
  在繁忙总线上让无关帧在硬件层被丢弃，几乎不占用 CPU。
- 只打印/记录控制帧日志；无关帧仅做原子计数，不做逐帧打印。
- 每秒检查 CAN 错误计数，变化时输出 `WARN` 诊断事件并强制记录状态快照。

## 架构

```mermaid
flowchart TD
    Init["CanCallback::init()"] --> Resistor["初始化终端电阻"]
    Resistor --> Driver["创建 HXC_TWAI<br/>读取 CAN_BAUDRATE"]
    Driver --> Filter["按 CAN_ID 配置硬件过滤器<br/>CAN_USE_HARDWARE_FILTER=1"]
    Filter --> Setup["setup()"]
    Setup --> Register["注册 4 个协议回调<br/>和 1 个计数 Catch-All"]
    Register --> Diag["创建 can_diag 任务"]

    Bus["CAN 总线帧"] --> HXC["HXC_TWAI<br/>按 identifier 分发"]
    HXC --> Callbacks["can_callback 业务回调"]
    Callbacks --> GS["global_state"]
    Callbacks --> Output["power_output"]
    Callbacks --> CR["can_resistor"]
    Callbacks --> Log["diagnostic_log<br/>ESP_LOG Hook 自动持久化"]
    Diag --> Log
```

回调执行过程已经在下方协议表中逐条列出，因此这里不再展开每个回调的时序图。

## CAN ID 规则

设备命令 ID 由基础设备 ID 和命令偏移相加得到：

```text
实际帧 ID = CAN_ID + CALLBACK_ID
```

| 偏移 | 名称 | 作用 |
|------|------|------|
| `0x00` | `CALLBACK_PING` | 原样回复收到的帧 |
| `0x01` | `CALLBACK_GET_STATE` | 返回 8 字节设备状态 |
| `0x02` | `CALLBACK_SET_OUTPUT` | 开关输出 |
| `0x03` | `CALLBACK_SET_RESISTOR` | 开关 CAN 终端电阻 |

默认 `CAN_ID` 为 `0x400`。`GET_STATE` 回复 ID 大于 `0x7ff` 时使用扩展帧，否则使用标准帧。

## 协议表

| 请求 ID | 请求数据 | 处理动作 | 回复 |
|---------|----------|----------|------|
| `CAN_ID + 0x00` | 任意 | 调用 `send(msg)` 原样回传 | 与请求相同 |
| `CAN_ID + 0x01` | 无要求 | 读取 `global_state` 和终端电阻状态 | `CALLBACK_GET_STATE_DATA_t` |
| `CAN_ID + 0x02` | `data[0] == 0x01` 表示开启，其他值表示关闭 | 校验 DLC 后异步调用 `PowerOutput::request()`，完成时记录最终结果 | 无 |
| `CAN_ID + 0x03` | `data[0] == 0x01` 表示开启，其他值表示关闭 | 调用 `CanResistor::set()`，并输出持久化诊断事件 | 无 |
| `-1` | 任意 | 全量 Catch-All：仅对收到的帧做原子计数，不打印 | 无 |

> 满载压测（6000~8000fps）时逐帧打印会成为瓶颈，因此 Catch-All 只计数，
> 累计值由 `can_diag` 每秒随诊断日志输出。控制帧的收发使用 `ESP_LOGI` 打印。

## 硬件过滤器

由编译期常量 `CAN_USE_HARDWARE_FILTER`（默认 `true`）控制。启用时在 `setup()` 前按 `CAN_ID`
配置硬件验收过滤器，只接收 `CAN_ID .. CAN_ID + CAN_CALLBACK_FILTER_SPAN - 1` 的本机控制帧。
`CAN_CALLBACK_FILTER_SPAN` 由枚举哨兵 `CALLBACK_ID_COUNT` 自动推导为“不小于它的最小 2 的幂”，
所以新增（连续偏移的）回调时过滤器会自动扩容，不会出现“新增回调被硬件过滤器误滤”的问题。

这与“设备挂在繁忙总线上、大量无关帧占用带宽”的场景配合，可显著降低 CPU 占用。

接收非本机定义的帧一般只在调试时需要，因此这里用编译期变量控制，不做运行期配置；
调试时将 `CAN_USE_HARDWARE_FILTER` 改为 `false` 重新编译即可接收总线上所有帧。

## 状态回复格式

`CALLBACK_GET_STATE_DATA_t` 使用 `packed` 布局，大小为 8 字节。CAN 单帧最多携带 8 字节，因此新增字段前必须检查大小。

| 字段 | 类型 | 单位或含义 |
|------|------|------------|
| `voltage_mV` | `uint16_t` | mV |
| `current_10mA` | `uint16_t` | 电流绝对值，单位 10mA，量程 0..655.35A |
| `Board_temperature` | `int8_t` | TMP235 板温，1 摄氏度 |
| `Chip_temperature` | `int8_t` | 芯片内温，1 摄氏度 |
| `output_state` | 1 bit | 输出状态 |
| `current_direction` | 1 bit | 代码中 `current_uA > 0` 时为 `1` |
| `CAN_resistor` | 1 bit | CAN 终端电阻状态 |
| `short_detect_running` | 1 bit | 短路检测是否正在执行 |
| `short_detect_passed` | 1 bit | 最近一次短路检测是否通过（未测过为 0） |
| `reserved` | 3 bit | 保留 |
| `UVP_flag` | 2 bit | 欠压保护状态 |
| `OVP_flag` | 2 bit | 过压保护状态 |
| `OTP_flag` | 2 bit | 过温保护状态 |
| `OCP_flag` | 2 bit | 过流保护状态 |

## NVS 配置

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `CAN_BAUDRATE` | `1_Mbps` | 初始化 TWAI 时读取，修改后需重新初始化或重启 |
| `CAN_ID` | `0x400` | 注册回调时读取，修改后需重新初始化或重启 |

> 硬件过滤器由编译期常量 `CAN_USE_HARDWARE_FILTER` 控制，不存 NVS。

## 使用方式

```cpp
#include "can_callback.h"

ESP_ERROR_CHECK(CanCallback::init());

if (CanCallback::is_available()) {
    HXC_TWAI& bus = CanCallback::get_can_bus();
    bus.send(&msg);
}
```

`get_can_bus()` 会直接解引用内部指针。只有 `init()` 成功或 `is_available()` 返回 `true` 后才能调用。

## 添加新命令

1. 在 `CALLBACK_ID` 中增加偏移值。
2. 在 `CanCallback::init()` 中调用 `add_can_receive_callback_func()` 注册回调。
3. 明确请求数据长度、回复格式和标准帧/扩展帧要求。
4. 若回复结构可能超过 8 字节，增加 `static_assert`。
5. 更新本 README 的协议表。
6. 新偏移请放在 `CALLBACK_ID_COUNT` 之前并保持连续：`CAN_CALLBACK_FILTER_SPAN` 会自动
   扩容到覆盖它的最小 2 的幂，注册前调用 `assert_callback_filtered<CALLBACK_XXX>()` 复用同一检查。

## 环境与依赖

- ESP-IDF v6.0+
- C++20

<!-- dependency-links:start -->
## 依赖导航

工程内直接依赖：

- [`global_state`](../global_state/README.md)（`app`）
- [`power_output`](../power_output/README.md)（`app`）
- [`protect`](../protect/README.md)（`app`）
- [`nvs_gpio_output`](https://github.com/qingmeijiupiao/wireless-power-components/blob/79d506e686ec743ad961ab76c732af96313db54a/components/bsp/nvs_gpio_output/README.md)（`bsp`）
- [`hardware`](../../bsp/hardware/README.md)（`bsp`）
- [`HXC_NVS`](https://github.com/qingmeijiupiao/wireless-power-components/blob/79d506e686ec743ad961ab76c732af96313db54a/components/bsp/HXC_NVS/README.md)（`bsp`）
- [`HXC_TWAI`](https://github.com/qingmeijiupiao/wireless-power-components/blob/79d506e686ec743ad961ab76c732af96313db54a/components/bsp/HXC_TWAI/README.md)（`bsp`）
- [`diagnostic_log`](https://github.com/qingmeijiupiao/wireless-power-components/blob/79d506e686ec743ad961ab76c732af96313db54a/components/common/diagnostic_log/README.md)（`common`）

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->

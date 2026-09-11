# ulp_app

`ulp_app` 运行在 ESP32-C6 的 LP Core 上。LP Core 可以理解为一个较轻量的辅助处理器：HP 核运行主业务、屏幕和网络，LP 核持续采样 INA228 并累计电量。

本目录只包含 LP 核程序。HP 核侧的加载、启动和校准参数搬运见 [`../ulp_loader/README.md`](../ulp_loader/README.md)。

## 设计目标

- 使用 LP I2C 持续读取 INA228，不占用 HP 核主任务。
- 在 LP 核上完成电流校准、温漂补偿、电量积分和能量积分。
- 通过 RTC 共享内存向 HP 核暴露测量值和状态。
- 使用整数运算，适应 LP 核环境。

## 整体架构

```mermaid
flowchart LR
    INA["INA228<br/>I2C 地址 0x40"] --> LP["LP Core ulp_app"]
    Calib["RTC 共享校准参数"] --> LP
    Board["HP 核写入板温<br/>0.01 摄氏度"] --> LP
    LP --> RTC["RTC 共享变量"]
    RTC --> HP["HP 核 app_main<br/>每 5ms 同步到 global_state"]
```

## 启动与主循环

```mermaid
flowchart TD
    Start["LP Core main()"] --> Load["加载 6 点插值参数"]
    Load --> Init["ulp_ina228_init()"]
    Init --> Reset["复位 INA228<br/>读取 manufacturer ID"]
    Reset --> Config["64 次平均<br/>BUS 与 SHUNT 连续转换"]
    Config --> First["等待第一次有效电压采样"]
    First --> InitOK["置位 ulp_ina228_init_ok"]
    InitOK --> Run["置位 ulp_run"]
    Run --> Loop["主循环"]
    Loop --> Timer["timer_run()<br/>更新内部毫秒计数"]
    Timer --> Sample["ina228_run()<br/>转换完成时更新测量值"]
    Timer --> Every1s["每约 1000ms<br/>更新循环频率"]
    Every1s --> Every20ms["每约 20ms<br/>检查校准参数重载"]
    Every20ms --> Every10ms["每约 10ms<br/>积分电量和能量"]
    Every10ms --> Loop
```

INA228 初始化和恢复阶段会阻塞重试 reset / 配置 / 首样本读取，直到首个有效样本成功。
重试期间置位 `ulp_i2c_init_err` 和 `ulp_ina228_read_timeout`，HP 核侧保护逻辑会据此暂停
OVP / UVP / OCP，不会因为无效测量强制关断输出。初始化成功后才置位 `ulp_run`。

## INA228 采样

INA228 配置为：

| 项目 | 配置 |
|------|------|
| I2C 地址 | `0x40` |
| 平均次数 | `64 samples` |
| 分流电压转换时间 | `1100 us` |
| 总线电压转换时间 | `1100 us` |
| 模式 | 分流电压与总线电压连续转换 |

主循环读取 MASK/ENABLE 寄存器的转换完成标志。只有转换完成时才更新电压和电流。
读寄存器失败或转换长时间未完成时，LP 核保留最后一次有效样本并置位
`ulp_ina228_read_timeout`，随后阻塞执行 INA228 重新初始化，直到恢复首个有效样本。

```mermaid
flowchart LR
    VRaw["BUS_VOLTAGE raw"] --> V["voltage_uv = raw * 1250"]
    IRaw["SHUNT_VOLTAGE raw"] --> Dead{"abs(raw * current_base_K)<br/>< 5000 ?"}
    Dead -->|是| Zero["current_uA = 0"]
    Dead -->|否| Base["raw * current_base_K"]
    Base --> Interp["+ 6 点插值补偿 * 100"]
    Interp --> Temp["减去温漂补偿"]
    Temp --> Current["current_uA"]
```

## 电流校准

校准参数类型来自 `current_calibration` 组件：

| 字段 | 作用 |
|------|------|
| `current_base_K` | 分流寄存器原始值到电流的基础比例 |
| `points[6]` | 6 个非等间距插值点，修正不同电流区间误差 |
| `temperature_K` | 温漂补偿系数 |

HP 核修改参数后，会置位 `ulp_reload_calib_params`。LP 核每约 20ms 检查一次该标志，重新加载插值表后清除标志。

`Board_temperature` 由 HP 核写入，单位是 `0.01 摄氏度`。LP 核先换算成整摄氏度温差，再进行补偿。

## 电量与能量积分

LP 核每约 10ms 调用 `update_meter()`：

```mermaid
flowchart TD
    Delta["计算 delta_ms"] --> Charge["累加 current_uA * delta_ms"]
    Delta --> Energy["累加 current_uA * voltage_uv * delta_ms"]
    Charge --> UAh{"跨过 3,600,000<br/>uA*ms 阈值?"}
    Energy --> UWh{"跨过 3,600,000,000,000<br/>uA*uV*ms 阈值?"}
    UAh --> MeterA["更新 meter_uah<br/>保留余数"]
    UWh --> MeterW["更新 meter_uwh<br/>保留余数"]
```

积分保留电流正负号，因此充电和放电方向会影响累计值。
当 INA228 测量处于初始化错误或读取超时状态时，LP 核会跳过本次积分并刷新积分时间基准，
避免用陈旧电压/电流继续累计电量。

## RTC 共享变量

带 `LP_VAR` 的变量位于 `.rtc.bss` 段，HP 核可直接访问。

| 变量 | 类型 | 单位 | 方向 | 说明 |
|------|------|------|------|------|
| `ulp_state` | `uint32_t` | - | LP -> HP，HP 可清零/置重载位 | 状态位集合 |
| `shared_lock` | `ulp_lp_core_spinlock_t` | - | HP <-> LP | 保护 RTC 共享变量快照 |
| `log_data` | `uint32_t` | - | LP -> HP | 预留 LP 日志数据 |
| `voltage_uv` | `uint32_t` | uV | LP -> HP | 总线电压 |
| `voltage_register_raw` | `uint16_t` | raw | LP -> HP | INA228 总线电压原始值 |
| `current_uA` | `int32_t` | uA | LP -> HP | 补偿后的电流 |
| `shunt_register_raw` | `int16_t` | raw | LP -> HP | INA228 分流电压原始值 |
| `ina228_manufacturer_id` | `uint16_t` | raw | LP -> HP | INA228 厂商 ID |
| `Board_temperature` | `int32_t` | 0.01 摄氏度 | HP -> LP | 板温，用于温漂补偿 |
| `meter_uah` | `int64_t` | uAh | LP -> HP | 累计电量 |
| `meter_uwh` | `int64_t` | uWh | LP -> HP | 累计能量 |
| `current_calib_params` | `CurrentCalib::params_t` | - | HP -> LP | 校准参数 |

## 状态位

| 位域 | 当前行为 |
|------|----------|
| `ulp_have_log` | 预留 LP 日志标志；当前主循环没有启用日志上报 |
| `ulp_i2c_init_err` | INA228 初始化或恢复中置位；首个有效样本恢复后清零 |
| `ulp_ina228_init_ok` | INA228 初始化和首个电压样本成功后置位 |
| `ulp_ina228_read_timeout` | INA228 连续 1 秒没有完整采样时置位；恢复采样后清零 |
| `ulp_run` | INA228 初始化成功后、进入主循环前置位 |
| `ulp_reload_calib_params` | HP 核请求重新加载校准参数时置位，LP 核处理后清除 |

## 文件说明

| 文件 | 作用 |
|------|------|
| `ulp_main.cpp` | LP 核入口、采样、补偿、积分和 RTC 共享变量 |
| `ina228.hpp` | LP I2C 版 INA228 寄存器访问和配置 |
| `ulp_Interp.hpp` | 固定容量非等间距插值器 |
| `ulp_state.h` | HP/LP 共用的状态位定义 |

## 注意事项

- LP 核侧尽量使用整数运算，新增逻辑前要评估代码体积和执行开销。
- HP 与 LP 之间通过 `shared_lock` 复制或提交 RTC 共享变量，持锁范围不包含 I2C 操作和插值计算。
- `voltage_uv` 是 uV，HP 核写入 `global_state` 时除以 `1000` 转换为 mV。
- `current_uA` 已包含死区、插值和温漂补偿，不是 INA228 原始寄存器值。
- INA228 连续 1 秒没有完整采样时会设置 `ulp_ina228_read_timeout`，但保留最后一次有效电压/电流；HP 侧保护逻辑会暂停 INA228 相关保护。
- `app_loop_every_ms()` 使用 `>` 判断间隔，因此文档使用“约 10ms / 20ms / 1000ms”描述。

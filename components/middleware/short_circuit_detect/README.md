# short_circuit_detect

PRO V2 输出端短路检测组件。组件通过短时拉高 `U_SHOT_TEST`，经硬件上的
二极管与 100 Ω 电阻向输出正极施加测试电压，并通过 `U_SHOT_DETECT` ADC
读取输出正极电压。两段测量都低于配置阈值时判定为短路。

已接入 `PowerOutput` 的开启前保护。产品调用统一由输出服务仲裁；Shell 手动测试也通过 `PowerOutput::test_short_circuit()`，输出已开启或已有检测事务时拒绝。

## 检测流程

检测分两段：第一段快速判定开路，第二段排除已连接负载拉低的“假短路”。

1. 应用层取得输出事务所有权，确保主输出关闭且检测期间不会开启。
2. 拉高 `U_SHOT_TEST`，等待 2ms 稳定。
3. 第一段：每 10ms 读取一次校准 ADC 电压，窗口 `FIRST_PROBE_MS`（500ms）；连续 3 次电压 >= 激励前取得的阈值快照即提前通过（开路）。
4. 若第一段未通过，拉低 `U_SHOT_TEST` 保持 `RELEASE_GAP_MS`（250ms），再拉高并用 `CONFIRM_WINDOW_MS`（250ms）复测；连续 3 次达标即通过（开路）。
5. 第 4 步反复执行，直到用完 `MAX_TEST_TIME_MS`（3000ms）总预算（约 5 个复测段）。启动电压低、上电瞬间就把测试电压拉低的器件/负载（含恒流、大电容、低阻负载），断开激励后有机会退出低阻态，从而在后续段恢复。
6. 全程各段都未通过才返回 `SHORT`。真实短路在各段始终保持低电平；好负载在第一段就提前通过，不增加等待。总预算、段窗口和释放间隔都是常量，可按负载特性调整。
7. ADC 读取失败或返回负电压时，将该次计入 `invalid_count` 并重试，但不打断已累计的达标计数（读取失败不代表短路）；只有有效低电压才清零达标计数，连续 3 次无效才返回检测错误。
8. 完成、错误或取消时拉低 `U_SHOT_TEST`。`voltage_mV` 为最后一次有效读数，`min_voltage_mV` 为整个检测期间最低有效电压，`sample_count` 只统计有效采样，判定不使用均值。

所有错误退出路径都会尝试关闭测试激励，避免 GPIO 长时间保持高电平。组件用原子占用标志拒绝并发检测，阈值读取和 NVS 写入使用互斥。底层不反向依赖应用输出模块，主输出互锁由 `PowerOutput` 保证。

启用“跳过保护”时不采样，但开启主输出前仍调用 `ensure_idle()` 确认测试激励可以拉低；清理失败不能旁路。

## 时序常量

| 常量 | 默认值 | 说明 |
|---|---:|---|
| `MAX_TEST_TIME_MS` | 3000ms | 检测总预算（最坏情况），供输出事务预算 |
| `FIRST_PROBE_MS` | 500ms | 第一段判定窗口 |
| `RELEASE_GAP_MS` | 250ms | 复测前断开激励的释放间隔 |
| `CONFIRM_WINDOW_MS` | 250ms | 每个复测段的窗口 |
| `SAMPLE_INTERVAL_MS` | 10ms | 采样间隔 |
| `REQUIRED_GOOD_SAMPLES` | 3 | 连续达标即判开路 |
| `MAX_CONSECUTIVE_INVALID_SAMPLES` | 5 | 连续无效即返回检测错误 |

## Shell 命令

```text
short_detect status
short_detect threshold
short_detect threshold 0.2
short_detect test
```

阈值命令使用伏特作为输入和显示单位。

## NVS

| Key | 类型 | 默认值 | 说明 |
|---|---|---:|---|
| `short_th_mv` | `uint16_t` | 200 mV | 短路判定阈值 |

## 公共 API

| API | 说明 |
|---|---|
| `init()` | 初始化组件并将测试引脚置低 |
| `test(result)` | 执行一次互斥测试；产品入口须由输出服务仲裁 |
| `ensure_idle()` | 与检测互斥，确认测试激励拉低 |
| `get_threshold_mV()` | 获取当前阈值 |
| `set_threshold_mV(value)` | 设置阈值并持久化到 NVS |

检测在采样间检查取消请求，关闭/保护取消后尽快结束测试激励；FreeRTOS 调度和 ADC 调用仍会影响实际墙钟时序。单次检测最长约 604ms，输出事务期限为 900ms，为两段检测另留调度、清理余量。


`invalid_count`（含已恢复的无效采样）、`min_voltage_mV`（窗口内最低有效电压）和 `duration_ms`（包含退出清理的调用耗时）供输出事务摘要和失败弹窗使用。逐次 ADC 重试和正常检测结果使用 DEBUG 日志，最终结果由 PowerOutput 统一记录。

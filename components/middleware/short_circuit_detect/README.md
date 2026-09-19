# short_circuit_detect

PRO V2 输出端短路检测组件。组件通过短时拉高 `U_SHOT_TEST`，经硬件上的
二极管与 100 Ω 电阻向输出正极施加测试电压，并通过 `U_SHOT_DETECT` ADC
读取输出正极电压。测量值低于配置阈值时判定为短路。

已接入 `PowerOutput` 的开启前保护。产品调用统一由输出服务仲裁；Shell 手动测试也通过 `PowerOutput::test_short_circuit()`，输出已开启或已有检测事务时拒绝。

## 检测流程

1. 应用层取得输出事务所有权，确保主输出关闭且检测期间不会开启。
2. 拉高 `U_SHOT_TEST`。
3. 等待 2 ms 稳定时间。
4. 每 10ms 读取一次校准 ADC 电压，测试窗口最长 500ms（含 2ms 稳定时间）。
5. 连续 3 次电压 >= 激励前取得的阈值快照即提前通过；任一次低于阈值将连续计数清零。
6. ADC 读取失败或返回负电压时，将该次计入 `invalid_count` 并重试，但不打断已累计的达标计数（读取失败不代表短路）；只有有效低电压才清零达标计数，连续 3 次无效才返回检测错误。
7. 达到 500ms 仍未满足条件则返回 `SHORT`；完成、错误或取消时拉低 `U_SHOT_TEST`。`voltage_mV` 为最后一次有效读数，`min_voltage_mV` 为窗口内最低有效电压，sample_count 只统计有效采样，不再使用均值判定。

所有错误退出路径都会尝试关闭测试激励，避免 GPIO 长时间保持高电平。组件用原子占用标志拒绝并发检测，阈值读取和 NVS 写入使用互斥。底层不反向依赖应用输出模块，主输出互锁由 `PowerOutput` 保证。

启用“跳过保护”时不采样，但开启主输出前仍调用 `ensure_idle()` 确认测试激励可以拉低；清理失败不能旁路。

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

检测在采样间检查取消请求，关闭/保护取消后尽快结束测试激励；FreeRTOS 调度和 ADC 调用仍会影响实际墙钟时序。输出事务期限为 750ms，为 500ms 检测另留调度、清理余量。


检测结果额外提供 `invalid_count`（含已恢复的无效采样）、`min_voltage_mV`（窗口内最低有效电压）和 `duration_ms`（包含退出清理的调用耗时），用于输出事务摘要和失败弹窗。逐次 ADC 重试及正常检测结果改为 DEBUG，由 PowerOutput 统一记录最终结果；判定门限、采样间隔和重试规则不变。

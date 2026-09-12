# short_circuit_detect

PRO V2 输出端短路检测组件。组件通过短时拉高 `U_SHOT_TEST`，经硬件上的
二极管与 100 Ω 电阻向输出正极施加测试电压，并通过 `U_SHOT_DETECT` ADC
读取输出正极电压。测量值低于配置阈值时判定为短路。

当前阶段仅供 Shell 手动测试，未接入主输出开启流程。

## 检测流程

1. 确保主输出处于关闭状态。
2. 拉高 `U_SHOT_TEST`。
3. 等待 2 ms 稳定时间。
4. 读取 4 次校准后的 ADC 电压，采样间隔 1 ms。
5. 立即拉低 `U_SHOT_TEST`。
6. 计算平均电压，低于阈值判定为 `SHORT`，否则为 `OPEN`。

所有错误退出路径都会尝试关闭测试激励，避免 GPIO 长时间保持高电平。

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
| `test(result)` | 执行一次测试并返回平均电压及判定结果 |
| `get_threshold_mV()` | 获取当前阈值 |
| `set_threshold_mV(value)` | 设置阈值并持久化到 NVS |

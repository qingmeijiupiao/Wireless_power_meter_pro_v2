# ADC

ADC 单次采样封装组件，基于 ESP-IDF oneshot ADC 与曲线拟合校准，提供 raw 值和校准后电压读取接口。

## 模块特点

- **ADC1 共享单元**：`adc_t` 使用静态 `adc1_unit_handle`，多个实例共享 ADC1 oneshot 单元
- **并发串行化**：所有实例通过内部互斥锁串行访问共享单元，避免并发读取时 `adc_oneshot_read` 的 try-lock 直接返回 `ESP_ERR_TIMEOUT`
- **按通道实例化**：构造时绑定 `adc_channel_t`，初始化时配置对应通道
- **曲线拟合校准**：通过 `adc_cali_create_scheme_curve_fitting()` 创建校准句柄
- **双读取接口**：支持读取原始 ADC raw 值，也支持换算为 mV

## 默认配置

| 配置 | 当前值 |
|------|--------|
| ADC 单元 | `ADC_UNIT_1` |
| 时钟 | `ADC_DIGI_CLK_SRC_DEFAULT` |
| ULP 模式 | `ADC_ULP_MODE_DISABLE` |
| 衰减 | `ADC_ATTEN_DB_12` |
| 位宽 | `ADC_BITWIDTH_DEFAULT` |

## 集成与使用

```cpp
#include "adc.h"

adc_t adc(ADC_CHANNEL_5);
ESP_ERROR_CHECK(adc.init());

int raw = 0;
ESP_ERROR_CHECK(adc.read_raw(raw));

int voltage_mV = 0;
ESP_ERROR_CHECK(adc.read_voltage_mV(voltage_mV));
```

## API 参考

| API | 说明 |
|-----|------|
| `adc_t(adc_channel_t channel)` | 创建并绑定 ADC 通道 |
| `init()` | 初始化 ADC1 oneshot 单元、配置通道并创建校准句柄 |
| `read_raw(int& raw)` | 读取原始 ADC raw 值 |
| `read_voltage_mV(int& voltage)` | 读取并换算为校准后的 mV 电压 |

## 注意事项

- 当前实现的校准配置中 `cali_config.chan` 初始化为 `ADC_CHANNEL_0`，如需多通道高精度校准，建议同步检查该配置是否需要按实例通道设置。
- 调用 `read_raw()` 或 `read_voltage_mV()` 前必须先调用 `init()`。
- 同一 ADC1 单元可能被多个任务并发读取。`adc_oneshot_read()` 内部用 try-lock 获取单元锁，拿不到会立即返回 `ESP_ERR_TIMEOUT`；本组件在 `read_raw()` 内已用互斥锁串行化，调用方无需重复加锁，也不要把 ADC 读取放到持锁时间很长的路径里。

## 环境与依赖

- **软件**：ESP-IDF v6.0+

<!-- dependency-links:start -->
## 依赖导航

无工程内组件依赖；仅依赖 ESP-IDF 组件或 C/C++ 标准库。

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->

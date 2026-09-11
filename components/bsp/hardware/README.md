# hardware

硬件版本识别与板级引脚配置组件，为应用层和 BSP 组件提供统一的硬件配置入口。

## 模块特点

- **硬件版本识别**：通过指定 ADC 通道读取硬件版本分压值，换算为硬件版本号
- **集中引脚表**：按硬件版本维护 TFT、CAN、INA228、温度传感器、输出控制和按键引脚
- **默认兜底配置**：未知版本会打印警告并回退到 `version_1`
- **启动前置依赖**：`hardware_config_init()` 必须在依赖引脚配置的模块初始化前调用

## 当前版本配置

当前源码内置 `version_1` 配置，覆盖以下硬件资源：

| 类型 | 配置项 |
|------|--------|
| TFT | `TFT_SCL`、`TFT_SDA`、`TFT_RST`、`TFT_RS`、`TFT_CS`、`TFT_BLK`、`TFT_BLK_ACTIVE_STATE` |
| 温度 | `temperature_channel` |
| CAN | `CAN_TX`、`CAN_RX`、`CAN_RESISTOR_ENABLE` |
| INA228 | `INAA226_SDA`、`INAA226_SCL`、`INAA226_ALERT` |
| 输出 | `OUTPUT_CTRL` |
| 按键 | `MAIN_BUTTON`、`SIDE_BUTTON` |

## 版本识别

```mermaid
flowchart LR
    Init["hardware_config_init()"] --> ADC["hardware_adc.init()"]
    ADC --> Sample["读取 10 次 ADC raw"]
    Sample --> Filter["剔除偏离均值的异常值"]
    Filter --> Calc["按 330 raw/档换算版本号"]
    Calc --> Store["保存 hardware_version"]
```

识别 ADC 通道由 `hardware_adc_channel` 定义。当前换算方式为 `(adc_value + 165) / 330 + 1`。

## 集成与使用

```cpp
#include "hardware.h"

ESP_ERROR_CHECK(hardware_config_init());

uint8_t version = get_hardware_version();
const hardware_config& cfg = get_hardware_config();
```

## API 参考

| API | 说明 |
|-----|------|
| `hardware_config_init()` | 初始化 ADC 并识别硬件版本 |
| `get_hardware_version()` | 返回硬件版本号，未识别前为 `255` |
| `get_hardware_config()` | 返回当前硬件版本对应的 `hardware_config` |

## 添加硬件版本

1. 新增一个 `hardware_config` 常量，填写该版本的完整引脚表。
2. 在 `get_hardware_config()` 的 `switch` 中增加对应版本分支。
3. 确认版本识别分压落在当前 ADC 换算规则可区分的范围内。

## 环境与依赖

- **硬件**：硬件版本识别分压接入 `hardware_adc_channel`
- **软件**：ESP-IDF v6.0+

<!-- dependency-links:start -->
## 依赖导航

工程内直接依赖：

- [`ADC`](../ADC/README.md)（`bsp`）

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->

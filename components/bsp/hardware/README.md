# hardware

硬件版本识别与板级引脚配置组件，为应用层和 BSP 组件提供统一的硬件配置入口。

## 模块特点

- **硬件版本识别**：通过指定 ADC 通道读取硬件版本分压值，换算为硬件版本号
- **集中引脚表**：按硬件版本维护 TFT、CAN、INA228、温度传感器、输出控制和按键引脚
- **默认兜底配置**：未知版本会打印警告并回退到 `version_0`
- **启动前置依赖**：`hardware_config_init()` 必须在依赖引脚配置的模块初始化前调用

## 当前版本配置

当前源码内置 `version_0` 配置，覆盖以下硬件资源：

| 类型 | 配置项 |
|------|--------|
| TFT | `TFT_SCL`、`TFT_SDA`、`TFT_RST`、`TFT_RS`、`TFT_CS`、`TFT_BLK`、`TFT_BLK_ACTIVE_STATE` |
| ADC | `temperature_channel`、`short_detect_channel` |
| CAN | `CAN_TX`、`CAN_RX`、`CAN_RESISTOR_ENABLE` |
| INA228 | `INA228_SDA`、`INA228_SCL`、`INA228_ALERT` |
| 输出 | `OUTPUT_CTRL`、`SHORT_TEST_ENABLE` |
| 按键 | `MAIN_BUTTON`、`SIDE_BUTTON`、`PREVIOUS_BUTTON` |

## 版本识别

```mermaid
flowchart LR
    Init["hardware_config_init()"] --> ADC["hardware_adc.init()"]
    ADC --> Sample["读取 10 次校准电压 mV"]
    Sample --> Filter["剔除偏离均值的异常值"]
    Filter --> Calc["按 330mV/档换算版本号"]
    Calc --> Store["保存 hardware_version"]
```

识别 ADC 通道由 `hardware_adc_channel` 定义。当前换算方式为 `(voltage_mV + 165) / 330`，0mV对应版本0，330mV对应版本1。先平均10个有效校准电压，再按最近档位取整；偏离运行均值超过165mV的样本重试，累计超过5次异常则返回错误并保留未知版本255。当前手板为0mV，仅配置版本0。

## 已确认手板参数

- 板温器件仍为TMP235，连接ADC_CHANNEL_3。
- 分流电阻默认2mΩ，对应当前兼容校准域的K=1250。
- INA228 ALERT连接GPIO4，外部已上拉；当前采样仍轮询DIAG_ALRT，未启用GPIO就绪通知。
- LP I2C在ulp_loader中固定使用GPIO6/7；引脚字段命名已统一为INA228。

## 集成与使用

### ALERT 数据就绪能力

固件未启用 ALERT GPIO 通知。INA228 可将转换完成状态输出到 `DIAG_ALRT`（0x0B）：`CNVR`（bit14）使能，`APOL=0` 为低有效开漏输出，与外部上拉连接相符；转换和平均完成后 `CNVRF`（bit1）置位。LP 侧通过轮询读取 `DIAG_ALRT` 并清除就绪标志，不通过 GPIO 通知。

依据：[TI INA228 数据手册](https://www.ti.com/lit/ds/symlink/ina228.pdf)，第7.3.4、7.3.7节及表7-16。

### 初始化示例

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
| `read_short_detect_raw()` | 读取短路检测 ADC 原始值 |
| `read_short_detect_voltage_mV()` | 读取校准后的短路检测电压，单位 mV |
| `set_short_test_enabled()` | 设置短路测试激励电平 |

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

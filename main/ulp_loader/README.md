# ulp_loader

HP 核侧 LP Core 加载模块，负责初始化 LP I2C、加载 `ulp_app` 编译出的二进制、启动 LP 核、搬运电流校准参数，并把 LP 核的原始寄存器共享变量指针暴露给调试命令。

## 模块特点

- **LP Core 启动封装**：`LP_Core_Load()` 完成 LP I2C 初始化、二进制加载、运行和启动状态检查
- **校准参数桥接**：从 `CurrentCalib::params_data` 读取 NVS 参数，写入 LP 核 RTC 共享变量
- **启动握手**：等待 `ulp_run` 与 `ulp_ina228_init_ok` 置位，最长约 600ms
- **LP 日志桥接**：后台任务轮询 `ulp_have_log`，将 LP 核日志值转为 HP 核 `ESP_LOGI`
- **共享快照**：通过 LP/HP 跨核锁一次性读取状态、采样值、原始寄存器和累计值

## 启动流程

```mermaid
sequenceDiagram
    participant Main as app_main
    participant Loader as LP_Core_Load()
    participant I2C as LP I2C
    participant RTC as RTC 共享内存
    participant LPBin as ulp_main.bin
    participant LP as LP 核
    participant NVS as CurrentCalib::params_data

    Main->>Loader: LP_Core_Load()
    Loader->>RTC: ulp_state_raw = 0
    Loader->>Loader: 切换 LP fast clock 到 20MHz 外部时钟源
    Loader->>I2C: lp_core_i2c_master_init()
    Loader->>LPBin: ulp_lp_core_load_binary()
    Loader->>LP: ulp_lp_core_run()
    Loader->>NVS: read()
    Loader->>RTC: 写入 current_calib_params
    loop 每 10ms，最多约 600ms
        Loader->>RTC: 检查 ulp_run 和 ulp_ina228_init_ok
    end
    Loader->>RTC: 绑定 raw register 指针
    Loader->>Loader: 创建 print_lp_core_log_task
    Loader-->>Main: ESP_OK / ESP_ERR_TIMEOUT
```

## 模块结构

```mermaid
flowchart TD
    CMake["main/CMakeLists.txt<br/>ulp_embed_binary(ulp_main)"] --> Symbols["bin_start / bin_end<br/>链接符号"]
    Symbols --> Loader["ulp_loader.cpp"]
    Loader --> I2C["LP I2C<br/>GPIO6 SDA / GPIO7 SCL / 400kHz"]
    Loader --> Core["ulp_lp_core_load_binary<br/>ulp_lp_core_run"]
    Loader --> Calib["load_current_calib_params()<br/>NVS -> RTC"]
    Loader --> State["ULP_CORE_STATE<br/>启动状态与日志标志"]
    State --> LogTask["print_lp_core_log_task<br/>10ms 轮询"]
```

## 关键数据关系

```mermaid
classDiagram
    class LP_Core_Load {
        +esp_err_t LP_Core_Load()
    }
    class ULP_CORE_STATE {
        +uint32_t ulp_state_raw
        +ulp_have_log : 1
        +ulp_i2c_init_err : 1
        +ulp_ina228_init_ok : 1
        +ulp_run : 1
        +ulp_reload_calib_params : 1
    }
    class CurrentCalib_params_t {
        +uint16_t current_base_K
        +point_t points[6]
        +int16_t temperature_K
    }
    class DebugPointers {
        +LP_Core_GetSnapshot()
        +LP_Core_SetBoardTemperature()
    }
    LP_Core_Load --> ULP_CORE_STATE
    LP_Core_Load --> CurrentCalib_params_t
    LP_Core_Load --> DebugPointers
```

## API 参考

| API | 说明 |
|-----|------|
| `LP_Core_Load()` | 初始化并启动 LP 核，成功后返回 `ESP_OK`，启动握手超时返回 `ESP_ERR_TIMEOUT` |

## 文件说明

| 文件 | 作用 |
|------|------|
| `ulp_loader.cpp` | LP 核加载、启动握手、校准参数搬运、LP 日志轮询 |
| `ulp_loader.h` | 对外声明 `LP_Core_Load()` |

## 注意事项

- 当前 `i2c_cfg` 固定使用 GPIO6/GPIO7，与 `hardware_config` 中 INA228 引脚保持一致；若未来硬件版本切换 INA228 引脚，需要同步调整这里。
- `LP_Core_Load()` 内部使用 `ESP_ERROR_CHECK` 处理 I2C、二进制加载和运行错误，相关错误会直接触发 ESP-IDF 错误检查行为。
- `LP_Core_GetSnapshot()` 使用 RTC 共享自旋锁，调用方不会读取到撕裂的 `int64_t` 累计值或不一致的采样字段。

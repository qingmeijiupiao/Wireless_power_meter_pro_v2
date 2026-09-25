# boot_diagnostics

应用启动诊断组件，负责将启动阶段、固件信息、硬件配置和运行期摘要写入黑匣子。

## API

| API | 说明 |
| --- | --- |
| `append_stage(stage)` | 记录当前启动阶段 |
| `append_hardware_config_failure(err)` | 记录硬件配置初始化失败 |
| `append_system_boot_start()` | 记录系统启动事件 |
| `append_early()` | 记录固件、Flash、MAC、CAN、WiFi、校准和保护阈值 |
| `append_runtime()` | 记录网络状态和 INA228 原始值 |

该组件只负责诊断记录。初始化顺序仍由 `main/app_main.cpp` 编排。

<!-- dependency-links:start -->
## 依赖导航

工程内直接依赖：

- [`blackbox_service`](../blackbox_service/README.md)（`app`）
- [`can_callback`](../can_callback/README.md)（`app`）
- [`current_calibration`](../current_calibration/README.md)（`app`）
- [`global_state`](../global_state/README.md)（`app`）
- [`protect`](../protect/README.md)（`app`）
- [`wifi_service`](../wifi_service/README.md)（`app`）
- [`blackbox`](https://github.com/qingmeijiupiao/wireless-power-components/blob/86e167a755eec29b38ce2f469fb3b6ba6a4fd0bb/components/middleware/blackbox/README.md)（`middleware`）
- [`can_resistor`](https://github.com/qingmeijiupiao/wireless-power-components/blob/86e167a755eec29b38ce2f469fb3b6ba6a4fd0bb/components/middleware/can_resistor/README.md)（`middleware`）
- [`hardware`](../../bsp/hardware/README.md)（`bsp`）

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->

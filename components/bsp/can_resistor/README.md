# can_resistor

CAN 终端电阻控制（板级门面）。封装终端电阻使能 GPIO、状态持久化和
`GlobalState` 同步，供 CAN 回调和屏幕设置页复用。

本组件是**板级**封装：引脚属于具体板卡，因此不放在公共组件库。通用能力
（带 NVS 持久化的输出 GPIO）由公共组件 `nvs_gpio_output` 提供，本组件只负责
绑定 NVS Key `can_term` 与单例接口。

## 行为

- `CanResistor::instance()` 返回单例控制器。
- `init(gpio_num)` 初始化 GPIO，并从 `can_term` NVS Key 恢复上次状态。
- `set()` 和 `toggle()` 修改 GPIO 后立即写入 NVS；持久化失败时返回错误并回滚 GPIO。
- `add_on_change_callback(callback)` 添加状态改变回调函数，用于同步更新 `GlobalState` 中的 `flags.can_resistor_enabled`；回调可在 `init()` 前注册，恢复状态时触发一次。

## API

| API | 说明 |
|-----|------|
| `instance()` | 获取单例控制器 |
| `init(gpio_num)` | 初始化 GPIO 并恢复持久化状态 |
| `set(enabled)` | 设置状态并持久化 |
| `toggle()` | 切换状态并持久化 |
| `get()` | 获取当前状态 |
| `add_on_change_callback(callback)` | 添加状态改变回调函数 |

## NVS Key

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `can_term` | blob(uint8_t) | `0` | `1` 表示接入 CAN 终端电阻 |

<!-- dependency-links:start -->
## 依赖导航

工程内直接依赖：

- [`nvs_gpio_output`](https://github.com/qingmeijiupiao/wireless-power-components/blob/3603981a47e4f29e3602806d900491508b692467/components/bsp/nvs_gpio_output/README.md)（`bsp`）

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->

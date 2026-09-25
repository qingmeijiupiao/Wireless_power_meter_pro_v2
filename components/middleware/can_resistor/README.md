# can_resistor

CAN 终端电阻控制中间件。封装终端电阻使能 GPIO、状态持久化和
`GlobalState` 同步，供 CAN 回调和屏幕设置页复用。

## 行为

- `CanResistor::instance()` 返回单例控制器。
- `init()` 初始化 GPIO，并从 `can_term` NVS Key 恢复上次状态。
- `set()` 和 `toggle()` 修改 GPIO 后立即写入 NVS；持久化失败时返回错误并回滚 GPIO。
- `add_on_change_callback(callback)` 添加状态改变回调函数，用于同步更新 `GlobalState` 中的 `flags.can_resistor_enabled`。

## API

| API | 说明 |
|-----|------|
| `instance()` | 获取单例控制器 |
| `init(gpio_num)` | 初始化 GPIO 并恢复持久化状态 |
| `set(enabled)` | 设置状态并持久化 |
| `toggle()` | 切换状态并持久化 |
| `get()` | 获取当前 GPIO 状态 |
| `add_on_change_callback(callback)` | 添加状态改变回调函数 |

## NVS Key

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `can_term` | blob(uint8_t) | `0` | `1` 表示接入 CAN 终端电阻 |

<!-- dependency-links:start -->
## 依赖导航

工程内直接依赖：

- [`cpp_gpio_driver`](https://github.com/qingmeijiupiao/wireless-power-components/blob/e73b43ee3637419643c1de325bbcfaa11c6b71c7/components/bsp/cpp_gpio_driver/README.md)（`bsp`）
- [`HXC_NVS`](../../bsp/HXC_NVS/README.md)（`bsp`）

> 本节按当前 `CMakeLists.txt` 的 `REQUIRES` / `PRIV_REQUIRES` 维护。
<!-- dependency-links:end -->

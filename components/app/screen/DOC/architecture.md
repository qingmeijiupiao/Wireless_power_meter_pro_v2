# Screen 架构设计

## 类关系

```mermaid
classDiagram
    class Page {
        <<abstract>>
        +id() PageId
        +title() const char*
        +refresh_interval_ms() uint32_t
        +handle_button(ButtonId, ButtonEvent) bool
        +render(RenderMode)
    }
    class UIManager {
        -Page* pages[]
        -QueueHandle_t event_queue
        +init() bool
        +post_button_event() bool
        +loop_once()
    }
    class PageRegistry {
        +Page** pages
        +size_t count
    }
    UIManager --> PageRegistry
    UIManager --> Page
    Page <|-- DashboardPage
    Page <|-- BatteryPage
    Page <|-- CurvePage
    Page <|-- WirelessPage
    Page <|-- SettingsPage
```

## 任务时序

```mermaid
sequenceDiagram
    participant App as app_main
    participant Screen as screen_task
    participant Manager as UIManager
    participant Page as Current Page
    participant LCD as ST7789
    App->>Screen: create task
    Screen->>LCD: init and show logo
    Screen->>Manager: init registry and queue
    loop UI loop
        Screen->>Manager: loop_once()
        Manager->>Manager: consume button queue
        Manager->>Page: handle_button()
        Manager->>Page: render()
        Manager->>LCD: sync_buffers()
    end
```

页面切换会依次调用旧页面 `on_edit_exit()`、`on_exit()` 和新页面 `on_enter()`。

## 事件分发

```mermaid
flowchart TD
    Input["ButtonMessage"] --> PageHandler["Page::handle_button"]
    PageHandler -->|已处理| Redraw["full_redraw = true"]
    PageHandler -->|未处理| ButtonType{"按键"}
    ButtonType -->|侧键短按| Next["next_page"]
    ButtonType -->|侧键长按| Edit["on_edit_enter"]
    ButtonType -->|主键短按| Output["PowerOutput::toggle"]
    Next --> Redraw
    Edit --> Redraw
    Output --> Redraw
```

页面事件优先于全局默认行为，使 Curve、Wireless 和 Settings 可以复用相同物理按键，
同时避免把页面专属状态写进 `UIManager`。

## 刷新调度

`UIManager` 使用 FreeRTOS tick 计算当前毫秒时间。页面切换或事件处理后忽略页面刷新
周期执行一次完整刷新；其他情况下按 `refresh_interval_ms()` 调度。未到刷新时间时任务
延迟 5ms，避免空转占用 HP Core。

曲线采样独立于当前页面，`CurveHistory::poll()` 每轮 UI 循环都会被调用，因此离开曲线页
后历史数据仍连续。

## 依赖边界

```mermaid
flowchart TB
    Public["include/screen.h"] --> Task["screen.cpp"]
    Task --> Core["core"]
    Core --> Abstract["Page abstraction"]
    Registry["page_registry.cpp"] --> Pages["concrete pages"]
    Pages --> Services["application/middleware services"]
    Pages --> Driver["ST7789"]
    Widgets["widgets"] --> Driver
    Config["config"] --> NVS["HXC_NVS"]
```

`core` 不包含具体业务服务头文件；具体页面可以依赖业务服务；`widgets` 只依赖显示驱动。

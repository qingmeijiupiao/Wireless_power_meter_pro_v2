/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 屏幕 UI 管理器实现，集中处理页面生命周期、按键事件队列和渲染调度
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-05-30
 */
#include "core/ui_manager.h"

#include "esp_log.h"
#include "core/page_registry.h"
#include "config/display_config.h"
#include "pages/curve/curve_history.h"
#include "freertos/task.h"
#include "power_output.h"
#include "st7789.h"
#include "widgets/ui_chrome.h"

namespace SCREEN {
namespace {

static constexpr char TAG[] = "UIManager";

const char* button_to_str(ButtonId button) {
    switch (button) {
    case ButtonId::Main: return "main";
    case ButtonId::Side: return "side";
    case ButtonId::Previous: return "previous";
    default: return "unknown";
    }
}

const char* event_to_str(ButtonEvent event) {
    switch (event) {
    case ButtonEvent::SHORT_PRESS:
        return "short";
    case ButtonEvent::DOUBLE_CLICK:
        return "double";
    case ButtonEvent::LONG_PRESS:
        return "long";
    case ButtonEvent::SUPER_LONG_PRESS:
        return "super_long";
    default:
        return "unknown";
    }
}

} // namespace

UIManager& UIManager::instance() {
    static UIManager manager;
    return manager;
}

bool UIManager::init() {
    if (event_queue_ == nullptr) {
        // 按键回调来自 Button 独立任务，事件先进入队列，再由 screen_task 串行消费。
        event_queue_ = xQueueCreate(8, sizeof(ButtonMessage));
        if (event_queue_ == nullptr) {
            ESP_LOGE(TAG, "create button event queue failed");
            return false;
        }
    }

    const PageRegistry registry = get_page_registry();
    if (registry.count != static_cast<size_t>(PageId::Count)) {
        ESP_LOGE(TAG, "invalid page registry count=%u", static_cast<uint32_t>(registry.count));
        return false;
    }
    for (size_t index = 0; index < registry.count; ++index) {
        pages_[index] = registry.pages[index];
    }
    current_page_ = 0;
    current_page()->on_enter();
    full_redraw_    = true;
    last_render_ms_ = 0;
    return true;
}

bool UIManager::post_button_event(ButtonId button, ButtonEvent event) {
    if (event_queue_ == nullptr) {
        // 队列尚未创建时返回失败，主按钮调用方会回退到直接控制输出。
        return false;
    }

    ButtonMessage msg = {
        .button = button,
        .event  = event,
    };
    return xQueueSend(event_queue_, &msg, 0) == pdTRUE;
}

void UIManager::apply_saved_display_config() {
    bool    rotate_180 = ui_config_get_rotation_180();
    uint8_t level      = ui_config_get_backlight_level();

    // 页面仍使用 160x80 逻辑坐标，旋转映射交给 ST7789 驱动处理。
    ST7789::set_rotation(rotate_180 ? ST7789::Rotation::HorizontalMirror : ST7789::Rotation::Horizontal);
    ST7789::set_backlight(backlight_value_from_level(level));
}

void UIManager::loop_once() {
    const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    CurveHistory::instance().poll(now_ms);
    process_button_events();

    // 页面可以声明自己的刷新周期；按键或切页会强制 full_redraw_，立即刷新。
    Page* page = current_page();
    if (!full_redraw_ && now_ms - last_render_ms_ < page->refresh_interval_ms()) {
        vTaskDelay(pdMS_TO_TICKS(5));
        return;
    }

    RenderMode mode = full_redraw_ ? RenderMode::Full : RenderMode::Normal;
    page->render(mode);
    if (page->is_overlay_active() && page->id() != PageId::Settings) {
        draw_edit_indicator();
    }

    // 当前页面实现均为整屏绘制，因此每帧直接同步当前缓冲即可。
    ST7789::sync_buffers();
    full_redraw_    = false;
    last_render_ms_ = now_ms;
}

Page* UIManager::current_page() {
    return pages_[current_page_];
}

void UIManager::process_button_events() {
    if (event_queue_ == nullptr) {
        return;
    }

    ButtonMessage msg = {};
    // 每轮尽量清空队列，避免连续按键时 UI 状态落后于输入。
    while (xQueueReceive(event_queue_, &msg, 0) == pdTRUE) {
        handle_button(msg.button, msg.event);
    }
}

void UIManager::handle_button(ButtonId button, ButtonEvent event) {
    Page* page = current_page();
    ESP_LOGI(TAG, "button page=%s button=%s event=%s", page->title(), button_to_str(button), event_to_str(event));

    // 页面优先处理事件。比如无线页长按进入配网，设置页消费菜单内侧键。
    bool handled = page->handle_button(button, event);
    if (handled) {
        // 页面消费事件后通常会改变局部状态，下一轮强制完整刷新一次。
        full_redraw_ = true;
        return;
    }

    if (button == ButtonId::Side) {
        handle_default_side_button(event);
    } else if (button == ButtonId::Main) {
        handle_default_main_button(event);
    } else if (button == ButtonId::Previous && event == ButtonEvent::SHORT_PRESS) {
        previous_page();
    }
}

void UIManager::handle_default_side_button(ButtonEvent event) {
    if (event == ButtonEvent::SHORT_PRESS) {
        // 默认侧键短按做单向循环翻页。
        next_page();
        return;
    }

    if (event == ButtonEvent::LONG_PRESS && current_page()->supports_edit_mode()) {
        // 只有显式声明支持编辑的页面才响应长按进入编辑态。
        current_page()->on_edit_enter();
        full_redraw_ = true;
        return;
    }

    if (event == ButtonEvent::SUPER_LONG_PRESS) {
        ESP_LOGI(TAG, "side super long press reserved");
    }
}

void UIManager::handle_default_main_button(ButtonEvent event) {
    if (event == ButtonEvent::SHORT_PRESS) {
        // 主按钮默认保持产品核心行为：切换输出状态。
        PowerOutput::toggle(TAG);
        full_redraw_ = true;
    }
}

void UIManager::next_page() {
    Page* page = current_page();
    // 切页时统一退出页面编辑态，避免设置页等页面把按键语义泄漏到下一页。
    page->on_edit_exit();
    page->on_exit();
    const char* previous_title = page->title();
    current_page_              = (current_page_ + 1) % static_cast<uint8_t>(PageId::Count);
    current_page()->on_enter();
    ESP_LOGI(TAG, "page %s -> %s", previous_title, current_page()->title());
    full_redraw_ = true;
}

void UIManager::previous_page() {
    Page* page = current_page();
    page->on_edit_exit();
    page->on_exit();
    const char* previous_title = page->title();
    current_page_ = current_page_ == 0 ? static_cast<uint8_t>(PageId::Count) - 1 : current_page_ - 1;
    current_page()->on_enter();
    ESP_LOGI(TAG, "page %s -> %s", previous_title, current_page()->title());
    full_redraw_ = true;
}

} // namespace SCREEN

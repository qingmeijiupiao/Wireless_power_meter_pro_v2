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
#include "core/ui_schedule.h"
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
    case ButtonEvent::PRESS:
        return "press";
    case ButtonEvent::RELEASE:
        return "release";
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
    const TickType_t now = xTaskGetTickCount();
    next_frame_tick_ = next_history_tick_ = resume_work_tick_ = now;
    screen_task_.store(xTaskGetCurrentTaskHandle());
    PowerOutput::set_event_notifier([] { UIManager::instance().request_redraw(); });
    return true;
}

bool UIManager::post_button_event(ButtonId button, ButtonEvent event) {
    // 弹窗关闭事件不依赖普通队列容量，避免队列满时回退成开启请求。
    if (protection_dialog_active_) {
        if (event == ButtonEvent::PRESS || event == ButtonEvent::SHORT_PRESS ||
            event == ButtonEvent::DOUBLE_CLICK || event == ButtonEvent::LONG_PRESS)
            dismiss_protection_requested_ = true;
        wake();
        return true;
    }
    if (event_queue_ == nullptr) {
        // 队列尚未创建时返回失败，主按钮调用方会回退到直接控制输出。
        return false;
    }

    ButtonMessage msg = {
        .button = button,
        .event  = event,
    };
    const bool posted = xQueueSend(event_queue_, &msg, 0) == pdTRUE;
    if (posted) wake();
    return posted;
}

void UIManager::wake() {
    if (const auto task = screen_task_.load()) xTaskNotifyGive(task);
}

void UIManager::request_redraw() {
    external_redraw_requested_ = true;
    wake();
}

void UIManager::apply_saved_display_config() {
    bool    rotate_180 = ui_config_get_rotation_180();
    uint8_t level      = ui_config_get_backlight_level();

    // 页面仍使用 240x135 逻辑坐标，旋转映射交给 ST7789 驱动处理。
    ST7789::set_rotation(rotate_180 ? ST7789::Rotation::HorizontalMirror : ST7789::Rotation::Horizontal);
    ST7789::set_backlight(backlight_value_from_level(level));
}

void UIManager::loop_once() {
    // 通知洪泛不能跳过恢复预算；事件保留在队列中，恢复后立即处理。
    // 恢复时间随实际工作耗时变化，未执行工作时无需额外等待。
    TickType_t now = xTaskGetTickCount();
    const TickType_t recovery = UiSchedule::remaining(now, resume_work_tick_);
    if (recovery) vTaskDelay(recovery);

    now = xTaskGetTickCount();
    const bool dialog_active = protection_dialog_active_.load();
    const bool dirty = dialog_active ? dialog_dirty_ : full_redraw_;
    TickType_t wait_ticks = UiSchedule::remaining(now, next_history_tick_);
    if (!dialog_active) {
        wait_ticks = std::min(wait_ticks, UiSchedule::remaining(now, next_frame_tick_));
        if (feedback_deadline_active_)
            wait_ticks = std::min(wait_ticks, UiSchedule::remaining(now, next_feedback_tick_));
        if (main_pressed_)
            wait_ticks = std::min(wait_ticks, UiSchedule::remaining(now, press_expires_tick_));
    }
    // 零等待也消费合并通知；此处到等待之间到达的新通知不会丢失。
    ulTaskNotifyTake(pdTRUE, dirty ? 0 : wait_ticks);

    const TickType_t work_started = xTaskGetTickCount();
    now = work_started;
    if (UiSchedule::due(now, next_history_tick_)) {
        CurveHistory::instance().poll(now * portTICK_PERIOD_MS);
        next_history_tick_ = now + UiSchedule::interval(CurveHistory::SAMPLE_INTERVAL_MS);
    }

    PowerOutput::FailureNotice notice{};
    if (PowerOutput::take_failure_notice(notice)) {
        const bool opening = !protection_dialog_active_.load();
        const bool changed = notice.result != protection_notice_.result ||
                             notice.voltage_mV != protection_notice_.voltage_mV ||
                             notice.threshold_mV != protection_notice_.threshold_mV ||
                             notice.error != protection_notice_.error;
        protection_notice_ = notice;
        protection_dialog_active_ = true;
        dialog_dirty_ = dialog_dirty_ || opening || changed;
        dialog_needs_background_ = dialog_needs_background_ || opening;
    }
    if (external_redraw_requested_.exchange(false) && !protection_dialog_active_) {
        full_redraw_ = true;
    }
    if (dismiss_protection_requested_.exchange(false)) {
        protection_dialog_active_ = false;
        main_pressed_ = false;
        dialog_dirty_ = false;
        xQueueReset(event_queue_);
        full_redraw_ = true;
        ESP_LOGI(TAG, "protection dialog dismissed");
    }
    if (protection_dialog_active_ && PowerOutput::get_state()) {
        protection_dialog_active_ = false;
        main_pressed_ = false;
        dialog_dirty_ = false;
        full_redraw_ = true;
    }
    process_button_events();
    update_output_feedback();

    Page* page = current_page();
    const TickType_t frame_started = xTaskGetTickCount();
    if (protection_dialog_active_) {
        if (dialog_dirty_) {
            // 弹窗期间冻结背景；帧缓冲保留合成画面，更新文字不需重绘背景。
            if (dialog_needs_background_) page->render(RenderMode::Full);
            UI::short_circuit_dialog(protection_notice_.result == PowerOutput::OutputResult::FAIL_SHORT_CIRCUIT,
                                     protection_notice_.voltage_mV, protection_notice_.threshold_mV);
            ST7789::copy_buffers();
            ST7789::sync_buffers();
            dialog_dirty_ = dialog_needs_background_ = false;
            full_redraw_ = false;
        }
    } else if (full_redraw_ || UiSchedule::due(frame_started, next_frame_tick_)) {
        page->render(full_redraw_ ? RenderMode::Full : RenderMode::Normal);
        if (page->is_overlay_active() && page->id() != PageId::Settings) draw_edit_indicator();
        UI::output_feedback_overlay(page->id() == PageId::Dashboard);
        ST7789::sync_buffers();
        full_redraw_ = false;
        next_frame_tick_ = UiSchedule::next_frame(frame_started, xTaskGetTickCount(),
                                                 UiSchedule::interval(page->refresh_interval_ms()));
    }
    const TickType_t finished = xTaskGetTickCount();
    resume_work_tick_ = finished + UiSchedule::recovery(finished - work_started);
}

void UIManager::update_output_feedback() {
    const TickType_t now = xTaskGetTickCount();
    if (main_pressed_ && UiSchedule::due(now, press_expires_tick_)) main_pressed_ = false;
    const auto view = output_feedback_.update(PowerOutput::snapshot(), now * portTICK_PERIOD_MS, main_pressed_);
    if (!OutputFeedback::equal(view, UI::output_view())) {
        const auto& previous = UI::output_view();
        const bool state_changed = view.state != previous.state || view.pressed != previous.pressed ||
                                   view.bypassed != previous.bypassed;
        // 快页面复用正常帧显示动画/倒计时；慢页面才需要额外的 100ms 状态帧。
        const bool refresh = state_changed || current_page()->refresh_interval_ms() > 100 ||
                             (view.detail[0] != 0) != (previous.detail[0] != 0);
        UI::set_output_view(view);
        if (!protection_dialog_active_ && refresh) full_redraw_ = true;
    }
    const uint32_t delay_ms = output_feedback_.next_update_ms();
    feedback_deadline_active_ = delay_ms != UINT32_MAX;
    if (feedback_deadline_active_) next_feedback_tick_ = now + UiSchedule::interval(delay_ms);
}

Page* UIManager::current_page() {
    return pages_[current_page_];
}

void UIManager::process_button_events() {
    if (event_queue_ == nullptr) {
        return;
    }

    ButtonMessage msg = {};
    // 有界消费，生产者持续入队也不能让一次 UI 工作无限延长。
    for (uint8_t count = 0; count < 8 && xQueueReceive(event_queue_, &msg, 0) == pdTRUE; ++count) {
        handle_button(msg.button, msg.event);
    }
}

void UIManager::handle_button(ButtonId button, ButtonEvent event) {
    if (protection_dialog_active_) {
        if (event == ButtonEvent::PRESS || event == ButtonEvent::SHORT_PRESS ||
            event == ButtonEvent::DOUBLE_CLICK || event == ButtonEvent::LONG_PRESS) {
            protection_dialog_active_ = false;
            main_pressed_ = false;
            xQueueReset(event_queue_);
            full_redraw_ = true;
            ESP_LOGI(TAG, "protection dialog dismissed button=%s", button_to_str(button));
        }
        return;
    }
    Page* page = current_page();
    // 主按键以消抖后的 PRESS 作为短按动作：立即触发动作并点亮按下反馈，
    // 不等待双击窗口；RELEASE 只结束反馈，其余手势不参与。
    if (button == ButtonId::Main) {
        if (event == ButtonEvent::PRESS) {
            main_pressed_ = !page->is_overlay_active();
            press_expires_tick_ = xTaskGetTickCount() + pdMS_TO_TICKS(1100);
            full_redraw_ = true;
        } else if (event == ButtonEvent::RELEASE) {
            if (main_pressed_) press_expires_tick_ = xTaskGetTickCount() + pdMS_TO_TICKS(300);
            return;
        } else {
            main_pressed_ = false;
            return;
        }
    }
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
    if (event == ButtonEvent::PRESS) {
        // 主按钮默认保持产品核心行为：按下即切换输出状态。
        PowerOutput::request(PowerOutput::OutputOperation::TOGGLE, TAG);
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

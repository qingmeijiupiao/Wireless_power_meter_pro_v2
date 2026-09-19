/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 屏幕 UI 管理器声明，负责页面切换、按键事件分发、刷新节奏和全局覆盖层
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-05-30
 */
#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "core/page.h"
#include "power_output.h"
#include <atomic>

namespace SCREEN {

/**
 * @brief 按键事件消息
 */
struct ButtonMessage {
    ButtonId    button = ButtonId::Main;           /**< 按键 ID */
    ButtonEvent event  = ButtonEvent::SHORT_PRESS; /**< 按键事件 */
};

/**
 * @brief 多页面 UI 管理器
 *
 * 管理器负责页面注册、单向循环切页、默认按键行为、事件队列、
 * 刷新周期和全局覆盖层。所有 UI 状态变更都在 screen_task 中消费，
 * 避免按键任务直接修改屏幕状态。
 */
class UIManager {
  public:
    /**
     * @brief 获取 UIManager 单例
     * @return UIManager 引用
     */
    static UIManager& instance();

    /**
     * @brief 初始化页面数组和按键事件队列
     * @return true 初始化成功
     */
    bool init();

    /**
     * @brief 投递按键事件
     * @param button 按键 ID
     * @param event 按键事件
     * @return true 表示事件已写入队列
     */
    bool post_button_event(ButtonId button, ButtonEvent event);

    /**
     * @brief 应用已保存的屏幕方向和背光配置
     */
    void apply_saved_display_config();

    /**
     * @brief UI 任务单次循环
     */
    void loop_once();

    /** 唤醒等待中的屏幕任务；通知可合并，实际数据仍由队列/服务持有。 */
    void wake();
    void request_redraw();

  private:
    /** @brief `UIManager` 接口。 */
    UIManager() = default;

    /**
     * @brief 获取当前页面对象
     * @return 当前页面指针
     */
    Page* current_page();

    /**
     * @brief 消费按键事件队列
     */
    void process_button_events();

    /**
     * @brief 分发按键事件到页面或默认处理逻辑
     * @param button 按键 ID
     * @param event 按键事件
     */
    void handle_button(ButtonId button, ButtonEvent event);

    /**
     * @brief 处理侧键默认行为
     * @param event 按键事件
     */
    void handle_default_side_button(ButtonEvent event);

    /**
     * @brief 处理主按键默认行为
     * @param event 按键事件
     */
    void handle_default_main_button(ButtonEvent event);

    /**
     * @brief 切换到下一个页面
     */
    void next_page();
    void previous_page();

    /** 按键事件队列，生产者为 Button 任务，消费者为 screen_task */
    QueueHandle_t event_queue_ = nullptr;

    /** 页面指针表，顺序即侧键短按翻页顺序 */
    Page* pages_[static_cast<uint8_t>(PageId::Count)] = {};

    /** 当前页面在 pages_ 中的索引 */
    uint8_t current_page_ = 0;

    TickType_t next_frame_tick_ = 0;
    TickType_t next_history_tick_ = 0;
    TickType_t resume_work_tick_ = 0;
    std::atomic<TaskHandle_t> screen_task_{nullptr};
    std::atomic<bool> external_redraw_requested_{false};
    bool dialog_dirty_ = false;
    bool dialog_needs_background_ = false;

    /** 请求重绘可提前于页面周期，但不能绕过 CPU 预算。 */
    bool full_redraw_ = true;
    PowerOutput::FailureNotice protection_notice_{};
    std::atomic<bool> protection_dialog_active_{false};
    std::atomic<bool> dismiss_protection_requested_{false};
};

} // namespace SCREEN

#endif

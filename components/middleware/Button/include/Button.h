/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 任务驱动型按键驱动
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-04-06 15:38:58
 */
#ifndef Button_H
#define Button_H

#include <functional>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// ==========================================
// 阈值参数定义 (单位: ms)
// ==========================================
constexpr uint32_t BTN_DEBOUNCE_MS     = 5;    /**< 消抖时间 */
constexpr uint32_t BTN_LONG_MS         = 1000; /**< 长按判定阈值 */
constexpr uint32_t BTN_SUPER_LONG_MS   = 3000; /**< 超长按阈值 */
constexpr uint32_t BTN_DOUBLE_CLICK_MS = 250;  /**< 双击间隔阈值 */
constexpr uint32_t BTN_SCAN_TICK_MS    = 10;   /**< 定时器轮询间隔 */

/** 按键事件枚举 */
enum class ButtonEvent {
    SHORT_PRESS = 0,  /**< 短按 */
    DOUBLE_CLICK,     /**< 双击 */
    LONG_PRESS,       /**< 长按 */
    SUPER_LONG_PRESS, /**< 超长按 */
    SHORT_THEN_LONG,  /**< 短按后长按 */
    RELEASE,          /**< 松开 */
    PRESS,           /**< 消抖后的按下边沿，仅用于即时反馈，不代表短按成立 */
    EVENT_MAX         /**< 枚举边界，用于数组长度 */
};

using ButtonCallback = std::function<void()>;

class Button {
  public:
    /** @brief 构造未初始化的按键对象。 */
    Button();
    /** @brief 停止定时器、任务并释放按键资源。 */
    ~Button();

    /**
     * @description: 初始化GPIO并启动按键驱动
     * @param gpio_num GPIO引脚号
     * @param active_low 是否低电平有效 (默认true)
     * @return ESP_OK on success, or an error code on failure
     */
    esp_err_t setup(gpio_num_t gpio_num, bool active_low = true);

    /**
     * @description: 动态绑定事件回调 (替换 std::map)
     * @param event 触发的事件类型
     * @param cb 回调函数内容
     */
    void bind_event(ButtonEvent event, ButtonCallback cb);

    /**
     * @description: 移除事件回调
     * @param event 触发的事件类型
     */
    void unbind_event(ButtonEvent event);

  private:
    // 内部状态集 (位域压缩)
    struct {
        uint8_t click_count   : 3; /**< 点击次数统计 */
        uint8_t is_long_sent  : 1; /**< 是否已发送长按事件 */
        uint8_t is_super_sent : 1; /**< 是否已发送超长按事件 */
    } _state;

    gpio_num_t _pin;
    bool       _active_low;
    uint32_t   _ticks;     /**< 按下持续时间计数 */
    uint32_t   _gap_ticks; /**< 松开间隔时间计数 */

    /** 回调函数数组：通过枚举下标访问，替代 std::map */
    ButtonCallback _callbacks[static_cast<int>(ButtonEvent::EVENT_MAX)] = {nullptr};

    // 异步处理组件
    TimerHandle_t _timer       = nullptr;
    QueueHandle_t _evt_queue   = nullptr;
    TaskHandle_t  _task_handle = nullptr;

    /** @brief 周期扫描定时器回调。 */
    static void _timer_callback(TimerHandle_t xTimer);
    /** @brief 按键事件分发任务入口。 */
    static void _event_task(void* arg);
    /** @brief 根据当前电平推进按键状态机。 */
    void        _run_state_machine();
    /** @return true 表示当前 GPIO 电平对应按下状态。 */
    bool        _is_pressed();
    /** @brief 将识别出的按键事件投递到事件队列。 */
    void        _post_event(ButtonEvent evt);
};

#endif

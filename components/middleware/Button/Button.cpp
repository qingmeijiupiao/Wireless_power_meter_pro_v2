#include "Button.h"
#include "esp_log.h"

namespace {
constexpr uint32_t BUTTON_EVENT_TASK_STACK_SIZE = 2048;
}

Button::Button() : _pin(GPIO_NUM_NC), _active_low(true), _ticks(0), _gap_ticks(0) {

    _state.click_count   = 0;
    _state.is_long_sent  = 0;
    _state.is_super_sent = 0;
}

Button::~Button() {
    if (_timer) {
        xTimerStop(_timer, 0);
        xTimerDelete(_timer, 0);
    }
    if (_task_handle) {
        vTaskDelete(_task_handle);
    }
    if (_evt_queue) {
        vQueueDelete(_evt_queue);
    }
}

esp_err_t Button::setup(gpio_num_t gpio_num, bool active_low) {
    _pin        = gpio_num;
    _active_low = active_low;

    gpio_config_t io_conf = {};
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    io_conf.mode          = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask  = (1ULL << _pin);
    io_conf.pull_down_en  = active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en    = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    esp_err_t ret         = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE("Button", "GPIO %d config failed: %s", gpio_num, esp_err_to_name(ret));
        return ret;
    }

    _evt_queue = xQueueCreate(5, sizeof(ButtonEvent));
    if (_evt_queue == nullptr) {
        ESP_LOGE("Button", "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    // 2. 独立任务只负责消费事件并调用轻量回调，避免占用不必要的常驻栈。
    if (xTaskCreate(_event_task, "btn_task", BUTTON_EVENT_TASK_STACK_SIZE, this, 3, &_task_handle) != pdPASS) {
        ESP_LOGE("Button", "Failed to create event task");
        return ESP_ERR_NO_MEM;
    }

    // 3. 创建扫描定时器
    _timer = xTimerCreate("btn_tmr", pdMS_TO_TICKS(BTN_SCAN_TICK_MS), pdTRUE, this, _timer_callback);
    if (_timer == nullptr) {
        ESP_LOGE("Button", "Failed to create scan timer");
        return ESP_ERR_NO_MEM;
    }
    if (xTimerStart(_timer, 0) != pdPASS) {
        ESP_LOGE("Button", "Failed to start scan timer");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI("Button", "Scan timer start");

    return ESP_OK;
}

void Button::bind_event(ButtonEvent event, ButtonCallback cb) {
    int index = static_cast<int>(event);
    if (index >= 0 && index < static_cast<int>(ButtonEvent::EVENT_MAX)) {
        _callbacks[index] = cb;
    }
}

void Button::unbind_event(ButtonEvent event) {
    int index = static_cast<int>(event);
    if (index >= 0 && index < static_cast<int>(ButtonEvent::EVENT_MAX)) {
        _callbacks[index] = nullptr;
    }
}

void Button::_post_event(ButtonEvent evt) {
    if (_evt_queue) {
        // 非阻塞发送，如果队列满则丢弃（正常操作不会满）
        xQueueSend(_evt_queue, &evt, 0);
    }
}

void Button::_event_task(void* arg) {
    Button*     self = static_cast<Button*>(arg);
    ButtonEvent evt;
    auto        event_index_to_name = [](int idx) {
        switch (idx) {
        case 0:
            return "SHORT_PRESS";
        case 1:
            return "DOUBLE_CLICK";
        case 2:
            return "LONG_PRESS";
        case 3:
            return "SUPER_LONG_PRESS";
        case 4:
            return "SHORT_THEN_LONG";
        case 5:
            return "RELEASE";
        case 6:
            return "PRESS";
        default:
            return "UNKNOWN_EVENT";
        }
    };
    while (true) {
        // 阻塞等待扫描结果，不消耗CPU
        if (xQueueReceive(self->_evt_queue, &evt, portMAX_DELAY)) {
            int index = static_cast<int>(evt);
            ESP_LOGD("Button", "Detected %s", event_index_to_name(index));
            // 内部直接区分状态执行回调
            if (self->_callbacks[index]) {
                self->_callbacks[index]();
            }
        }
    }
}

void Button::_timer_callback(TimerHandle_t xTimer) {
    Button* instance = static_cast<Button*>(pvTimerGetTimerID(xTimer));
    instance->_run_state_machine();
}

bool Button::_is_pressed() {
    return gpio_get_level(_pin) == (_active_low ? 0 : 1);
}

void Button::_run_state_machine() {
    bool now_pressed = _is_pressed();

    if (now_pressed) {
        const uint32_t previous_ticks = _ticks;
        _ticks     += BTN_SCAN_TICK_MS;
        if (previous_ticks <= BTN_DEBOUNCE_MS && _ticks > BTN_DEBOUNCE_MS)
            _post_event(ButtonEvent::PRESS);
        _gap_ticks  = 0; // 按下期间，强制重置空闲计时

        // 1. 长按 & 短按再长按 判定
        if (_ticks >= BTN_LONG_MS && !_state.is_long_sent) {
            if (_state.click_count == 1) {
                _post_event(ButtonEvent::SHORT_THEN_LONG);
            } else {
                _post_event(ButtonEvent::LONG_PRESS);
            }
            _state.is_long_sent = 1;

            // 触发长按类事件后清空连击计数，避免松手时误触发短按或双击。
            _state.click_count = 0;
        }

        // 2. 超长按判定
        if (_ticks >= BTN_SUPER_LONG_MS && !_state.is_super_sent) {
            _post_event(ButtonEvent::SUPER_LONG_PRESS);
            _state.is_super_sent = 1;
            _state.click_count   = 0; // 保底清空
        }
    } else {
        // 3. 松开瞬间的边缘检测
        // 仅当 _ticks > 0（说明刚刚松开）时才执行释放判定，避免按钮空闲时反复重置变量
        if (_ticks > 0) {
            if (_ticks > BTN_DEBOUNCE_MS) {
                // 如果在松开前没有触发过任何长按，说明这是一次有效的短促点击
                if (!_state.is_long_sent) {
                    _state.click_count++;
                }
                _post_event(ButtonEvent::RELEASE);
            }

            // 仅在确认完全松开的瞬间重置按压状态
            _ticks               = 0;
            _state.is_long_sent  = 0;
            _state.is_super_sent = 0;
        }

        // 4. 空闲超时判定（用于确认单击或多击是否结束）
        if (_state.click_count > 0) {
            _gap_ticks += BTN_SCAN_TICK_MS;
            if (_gap_ticks > BTN_DOUBLE_CLICK_MS) {
                if (_state.click_count == 1) {
                    _post_event(ButtonEvent::SHORT_PRESS);
                } else {
                    _post_event(ButtonEvent::DOUBLE_CLICK); // 处理双击及以上情况
                }
                _state.click_count = 0; // 判定完毕，计数清零
                _gap_ticks         = 0;
            }
        }
    }
}

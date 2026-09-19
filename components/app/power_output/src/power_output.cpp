/** 统一输出事务；所有主输出开启必须经过本模块。 */
#include "power_output.h"
#include "protect_policy.hpp"
#include "cooldown_policy.hpp"
#include "short_circuit_detect.h"
#include "global_state.h"
#include "protect.h"
#include "diagnostic_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <array>
#include <atomic>
#include <utility>
#include <cstdio>
#include <cinttypes>

namespace PowerOutput {
namespace {
constexpr char TAG[] = "PowerOutput";
constexpr size_t MAX_POLICIES = 8;
constexpr size_t MAX_CALLBACKS = 8;
SemaphoreHandle_t transaction_mutex = nullptr;
SemaphoreHandle_t completed_signal = nullptr;
TaskHandle_t worker_task = nullptr;
std::atomic<bool> initialized{false};
gpio_num_t output_gpio = GPIO_NUM_NC;
std::array<OutputPolicy*, MAX_POLICIES> policies{};
size_t policy_count = 0;
std::array<OnOutputChangeCallback, MAX_CALLBACKS> change_callbacks{};
size_t callback_count = 0;
ProtectPolicy protect_policy;
CooldownPolicy cooldown_policy(OUTPUT_ON_COOLDOWN_MS, OUTPUT_OFF_COOLDOWN_MS);
bool protect_callback_registered = false;

// 只保留一个开启事务，OFF 后不会再排队执行旧的开启请求。同步调用超时后，
// 工作任务继续持有事务槽，直到测试激励清理完成。
struct PendingRequest {
    bool used = false;
    bool synchronous = false;
    bool diagnostic = false;
    bool cancelled = false;
    bool completed = false;
    int64_t deadline_us = 0;
    int64_t started_us = 0;
    uint32_t request_id = 0;
    OutputOperation operation = OutputOperation::ON;
    bool previous_state = false;
    bool timed_out = false;
    const char* source = "unknown";
    CompletionCallback completion;
    OutputResult result = OutputResult::OK;
    ShortCircuitDetect::Result measurement{};
};
PendingRequest pending;
Status latest_status;
std::atomic<uint32_t> request_sequence{0};
FailureNotice failure_notice;
bool failure_pending = false;
std::atomic<void (*)()> event_notifier{nullptr};

void notify_event() {
    if (const auto callback = event_notifier.load()) callback();
}

class Lock {
  public:
    Lock() { xSemaphoreTake(transaction_mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(transaction_mutex); }
};

void notify_change(bool state) {
    notify_event();
    std::array<OnOutputChangeCallback, MAX_CALLBACKS> callbacks;
    size_t count;
    {
        Lock lock;
        callbacks = change_callbacks;
        count = callback_count;
    }
    for (size_t i = 0; i < count; ++i) callbacks[i](state);
}

// 调用方持有事务锁；GPIO 成功后才发布真实输出状态。
OutputResult apply_state(bool state) {
    if (gpio_set_level(output_gpio, state ? 1 : 0) != ESP_OK) return OutputResult::FAIL_GPIO;
    update_global_state([state](GlobalState& global) { global.flags.output_enabled = state; });
    for (size_t i = 0; i < policy_count; ++i) {
        policies[i]->on_state_applied(state ? OutputOperation::ON : OutputOperation::OFF, state);
    }
    return OutputResult::OK;
}

OutputResult check_policies() {
    for (size_t i = 0; i < policy_count; ++i) {
        const auto result = policies[i]->check(OutputOperation::ON, false);
        if (result != OutputResult::OK) return result;
    }
    return OutputResult::OK;
}

// 只在事务锁内发布；较早请求的取消结果不能覆盖后续 OFF/新请求。
void publish_status(uint32_t id, OutputResult result) {
    latest_status.request_id = id;
    latest_status.result = result;
    latest_status.checking = result == OutputResult::PENDING;
}

uint8_t protection_mask() {
    const auto state = get_global_state().protect_states.states_bit;
    return (state.temperature_protect_state == PROTECT_STATE_PROTECT ? 1 : 0) |
           (state.high_voltage_protect_state == PROTECT_STATE_PROTECT ? 2 : 0) |
           (state.low_voltage_protect_state == PROTECT_STATE_PROTECT ? 4 : 0) |
           (state.current_protect_state == PROTECT_STATE_PROTECT ? 8 : 0);
}

// 每个请求只在仲裁层记录一次摘要；幂等操作和正常拒绝不追加状态快照。
void log_result(const char* source, uint32_t id, OutputOperation operation, OutputResult result,
                bool before, bool state, int64_t started_us, const ShortCircuitDetect::Result& test,
                esp_err_t error, bool tested, bool bypassed, uint32_t cooldown_ms, uint8_t mask) {
    char line[256];
    const char* op = operation == OutputOperation::ON ? "on" : operation == OutputOperation::OFF ? "off" : "toggle";
    snprintf(line, sizeof(line),
             "output id=%" PRIu32 " src=%s op=%s %u>%u result=%s ms=%" PRId64
             " test=%u test_ms=%" PRIu32 " mv=%u vmin=%u min=%u n=%u bad=%u err=%s bypass=%u wait=%" PRIu32 " protect=%u",
             id, source, op, before, state, result_to_string(result),
             (esp_timer_get_time() - started_us) / 1000, tested, test.duration_ms, test.voltage_mV,
             test.min_voltage_mV, test.threshold_mV,
             test.sample_count, test.invalid_count, esp_err_to_name(result == OutputResult::FAIL_GPIO ? ESP_FAIL : error), bypassed, cooldown_ms, mask);
    if (result == OutputResult::FAIL_SHORT_CIRCUIT || result == OutputResult::FAIL_SHORT_DETECT ||
        result == OutputResult::FAIL_TIMEOUT || result == OutputResult::FAIL_GPIO) {
        ESP_LOGW(TAG, "%s", line);
    } else if (before != state) {
        DEVICE_STATE_I(TAG, "%s", line);
    } else if (result != OutputResult::OK) {
        DEVICE_EVENT_I(TAG, "%s", line);
    } else {
        ESP_LOGI(TAG, "%s", line);
    }
}

void worker(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool diagnostic;
        bool should_test;
        OutputResult result;
        {
            Lock lock;
            if (!pending.used || pending.completed) continue;
            diagnostic = pending.diagnostic;
            result = pending.cancelled || !initialized ? OutputResult::FAIL_CANCELLED : OutputResult::OK;
            if (result == OutputResult::OK && esp_timer_get_time() >= pending.deadline_us)
                result = OutputResult::FAIL_TIMEOUT;
            if (result == OutputResult::OK && !diagnostic) result = check_policies();
            should_test = diagnostic || !protect_is_bypassed();
        }

        ShortCircuitDetect::Result measurement{};
        esp_err_t error = ESP_OK;
        bool tested = false;
        bool changed = false;
        bool published_failure = false;
        bool state = false;
        CompletionCallback completion;
        const char* source;
        uint32_t id = 0, cooldown_ms = 0;
        uint8_t mask = 0;
        int64_t started_us = 0;
        OutputOperation operation = OutputOperation::ON;
        bool before = false, bypassed = false;
        for (;;) {
            if (result == OutputResult::OK) {
                // 旁路也必须清理激励，避免测试电源与主输出同时开启。
                if (should_test) {
                    error = ShortCircuitDetect::test(measurement, [](void*) {
                        Lock lock;
                        return pending.cancelled || !initialized;
                    });
                    tested = true;
                } else {
                    error = ShortCircuitDetect::ensure_idle();
                }
                if (error != ESP_OK) result = OutputResult::FAIL_SHORT_DETECT;
                else if (tested && measurement.is_short) result = OutputResult::FAIL_SHORT_CIRCUIT;
            }

            Lock lock;
            ProtectOutputGuard protection_guard;
            if (pending.timed_out) result = OutputResult::FAIL_TIMEOUT;
            else if (pending.cancelled || !initialized) result = OutputResult::FAIL_CANCELLED;
            else if (esp_timer_get_time() >= pending.deadline_us) result = OutputResult::FAIL_TIMEOUT;
            // 检测中启用旁路可跳过短路判定，但不能忽略硬件清理失败。
            if (!diagnostic && result == OutputResult::FAIL_SHORT_CIRCUIT && protect_is_bypassed())
                result = OutputResult::OK;
            if (!diagnostic && result == OutputResult::OK) {
                result = check_policies();
                // 工作期间关闭旁路时，提交前必须补做检测。
                if (result == OutputResult::OK && !tested && !protect_is_bypassed()) {
                    should_test = true;
                    continue;
                }
                if (result == OutputResult::OK) {
                    result = apply_state(true);
                    changed = result == OutputResult::OK;
                }
            }
            source = pending.source;
            id = pending.request_id;
            started_us = pending.started_us;
            operation = pending.operation;
            before = pending.previous_state;
            bypassed = protect_is_bypassed();
            cooldown_ms = cooldown_policy.remaining_ms();
            mask = protection_mask();
            if (!diagnostic && latest_status.request_id == id) publish_status(id, result);
            state = get_state();
            if (!diagnostic && !pending.timed_out && latest_status.request_id == id && (result == OutputResult::FAIL_SHORT_CIRCUIT ||
                                result == OutputResult::FAIL_SHORT_DETECT || result == OutputResult::FAIL_TIMEOUT)) {
                // 弹窗展示窗口内最低有效电压，与“低于门限即短路”的判定一致。
                failure_notice = {result, measurement.min_voltage_mV, measurement.threshold_mV, error};
                failure_pending = true;
                published_failure = true;

            }
            if (pending.synchronous) {
                pending.result = result;
                pending.measurement = measurement;
                pending.completed = true;
                xSemaphoreGive(completed_signal);
            } else {
                completion = std::move(pending.completion);
                pending = {};
            }
            break;
        }
        // 业务回调必须在事务锁外执行。
        if (!diagnostic || published_failure) notify_event();
        log_result(source, id, operation, result, before, state, started_us, measurement,
                   error, tested, bypassed, cooldown_ms, mask);
        if (changed) notify_change(true);
        if (completion) completion(result, state);
    }
}

OutputResult submit(OutputOperation op, const char* source, CompletionCallback completion,
                    bool synchronous, bool diagnostic) {
    source = source ? source : "unknown";
    const uint32_t id = ++request_sequence;
    const int64_t started_us = esp_timer_get_time();
    uint32_t cooldown_ms = 0;
    uint8_t mask = 0;
    bool bypassed = false;
    OutputResult result = OutputResult::FAIL_NOT_INIT;
    bool changed = false;
    bool state = get_state();
    bool before = state;
    if (initialized.load()) {
        Lock lock;
        state = get_state();
        before = state;
        if (!initialized) {
            result = OutputResult::FAIL_NOT_INIT;
        } else if (diagnostic && (state || pending.used)) {
            result = OutputResult::FAIL_BUSY;
        } else if (!diagnostic && (op == OutputOperation::OFF ||
                   (op == OutputOperation::TOGGLE && (state || (pending.used && !pending.diagnostic))))) {
            if (pending.used && !pending.completed) pending.cancelled = true;
            result = state ? apply_state(false) : OutputResult::OK;
            changed = state && result == OutputResult::OK;
            state = get_state();
        } else if (!diagnostic && state) {
            result = OutputResult::OK;
        } else if (pending.used) {
            result = OutputResult::FAIL_BUSY;
        } else {
            result = diagnostic ? OutputResult::OK : check_policies();
            if (result == OutputResult::OK) {
                xSemaphoreTake(completed_signal, 0);
                pending = {};
                pending.used = true;
                pending.synchronous = synchronous;
                pending.diagnostic = diagnostic;
                pending.source = source;
                pending.request_id = id;
                pending.started_us = started_us;
                pending.operation = op;
                pending.previous_state = state;
                pending.deadline_us = esp_timer_get_time() + static_cast<int64_t>(REQUEST_TIMEOUT_MS) * 1000;
                pending.completion = std::move(completion);
                if (!diagnostic) {
                    failure_pending = false;
                    publish_status(id, OutputResult::PENDING);
                }
                xTaskNotifyGive(worker_task);
                result = OutputResult::PENDING;
            }
        }
        cooldown_ms = cooldown_policy.remaining_ms();
        bypassed = protect_is_bypassed();
        mask = protection_mask();
        if (!diagnostic && result != OutputResult::PENDING &&
            !(result == OutputResult::FAIL_BUSY && latest_status.checking)) {
            publish_status(id, result);
            if (result == OutputResult::OK) failure_pending = false;
        }
    }
    // 唤醒/业务回调和日志全部在事务锁外执行。
    if (!diagnostic) notify_event();
    if (result == OutputResult::PENDING) return result;
    if (changed) notify_change(false);
    log_result(source, id, op, result, before, state, started_us, {}, ESP_OK, false, bypassed, cooldown_ms, mask);
    if (completion) completion(result, state);
    return result;
}

OutputResult wait_result(ShortCircuitDetect::Result* measurement = nullptr) {
    xSemaphoreTake(completed_signal, pdMS_TO_TICKS(REQUEST_TIMEOUT_MS));
    bool published_failure = false;
    {
        Lock lock;
        if (pending.completed) {
            const auto result = pending.result;
            if (measurement) *measurement = pending.measurement;
            pending = {};
            return result;
        }
        // 事务数据由工作任务持有，超时不会留下指向调用者栈的指针。
        pending.cancelled = true;
        pending.timed_out = true;
        pending.synchronous = false;
        if (!pending.diagnostic && latest_status.request_id == pending.request_id) {
            publish_status(pending.request_id, OutputResult::FAIL_TIMEOUT);
            failure_notice = {OutputResult::FAIL_TIMEOUT, 0, 0, ESP_ERR_TIMEOUT};
            failure_pending = true;
            published_failure = true;
        }
    }
    if (published_failure) notify_event();
    return OutputResult::FAIL_TIMEOUT;
}

OutputResult synchronous_request(OutputOperation op, const char* source) {
    if (worker_task && xTaskGetCurrentTaskHandle() == worker_task) return OutputResult::FAIL_BUSY;
    const auto result = submit(op, source, {}, true, false);
    return result == OutputResult::PENDING ? wait_result() : result;
}
} // namespace

const char* result_to_string(OutputResult result) {
    switch (result) {
    case OutputResult::OK: return "ok";
    case OutputResult::FAIL_NOT_INIT: return "not_initialized";
    case OutputResult::FAIL_PROTECT_ACTIVE: return "protect_active";
    case OutputResult::FAIL_COOLDOWN_ACTIVE: return "cooldown_active";
    case OutputResult::FAIL_SHORT_CIRCUIT: return "short_circuit";
    case OutputResult::FAIL_SHORT_DETECT: return "short_detect_failed";
    case OutputResult::FAIL_BUSY: return "busy";
    case OutputResult::FAIL_CANCELLED: return "cancelled";
    case OutputResult::FAIL_TIMEOUT: return "timeout";
    case OutputResult::FAIL_GPIO: return "gpio_failed";
    case OutputResult::PENDING: return "pending";
    default: return "unknown";
    }
}

esp_err_t init(gpio_num_t gpio) {
    // app_main 负责初始化；反初始化保留任务资源，供在途检测清理激励。
    if (!transaction_mutex) transaction_mutex = xSemaphoreCreateMutex();
    if (!completed_signal) completed_signal = xSemaphoreCreateBinary();
    if (!transaction_mutex || !completed_signal) return ESP_ERR_NO_MEM;
    Lock lock;
    if (initialized) return ESP_OK;
    if (pending.used) return ESP_ERR_INVALID_STATE;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) return ESP_ERR_INVALID_ARG;
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << gpio;
    config.mode = GPIO_MODE_OUTPUT;
    esp_err_t error = gpio_config(&config);
    if (error != ESP_OK) return error;
    output_gpio = gpio;
    error = gpio_set_level(gpio, 0);
    if (error != ESP_OK) return error;
    update_global_state([](GlobalState& state) { state.flags.output_enabled = false; });
    cooldown_policy.reset();
    latest_status = {};
    failure_pending = false;
    policy_count = 0;
    policies[policy_count++] = &protect_policy;
    policies[policy_count++] = &cooldown_policy;
    if (!worker_task && xTaskCreate(worker, "output_check", 4096, nullptr, 4, &worker_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (!protect_callback_registered) {
        add_on_protect_change_callback([](ProtectState_t, ProtectState_t next) {
            if (next == PROTECT_STATE_PROTECT && protect_should_block_output()) off("protect");
        });
        protect_callback_registered = true;
    }
    initialized = true;
    return ESP_OK;
}

esp_err_t deinit() {
    if (!initialized) return ESP_OK;
    bool changed;
    {
        Lock lock;
        pending.cancelled = true;
        changed = get_state();
        if (apply_state(false) != OutputResult::OK) return ESP_FAIL;
        initialized = false;
    }
    if (changed) notify_change(false);
    return ESP_OK;
}

OutputResult request(OutputOperation op, const char* source, CompletionCallback completion) {
    return submit(op, source, std::move(completion), false, false);
}
OutputResult on(const char* source) { return synchronous_request(OutputOperation::ON, source); }
OutputResult off(const char* source) { return submit(OutputOperation::OFF, source, {}, false, false); }
OutputResult toggle(const char* source) { return synchronous_request(OutputOperation::TOGGLE, source); }
bool get_state() { return get_global_state().flags.output_enabled; }

OutputResult test_short_circuit(ShortCircuitDetect::Result& result, const char* source) {
    result = {};
    if (worker_task && xTaskGetCurrentTaskHandle() == worker_task) return OutputResult::FAIL_BUSY;
    const auto submitted = submit(OutputOperation::ON, source, {}, true, true);
    return submitted == OutputResult::PENDING ? wait_result(&result) : submitted;
}

Status snapshot() {
    if (!initialized.load()) return {};
    Lock lock;
    ProtectOutputGuard protection_guard;
    Status status = latest_status;
    status.initialized = initialized.load();
    status.output_on = get_state();
    status.bypassed = protect_is_bypassed();
    status.protect_mask = protection_mask();
    status.cooldown_remaining_ms = cooldown_policy.remaining_ms();
    return status;
}

bool take_failure_notice(FailureNotice& notice) {
    if (!initialized) return false;
    Lock lock;
    if (!failure_pending) return false;
    notice = failure_notice;
    failure_pending = false;
    return true;
}

void set_event_notifier(void (*notifier)()) { event_notifier.store(notifier); }

void add_on_change_callback(OnOutputChangeCallback callback) {
    if (!transaction_mutex || !callback) return;
    Lock lock;
    if (callback_count < MAX_CALLBACKS) change_callbacks[callback_count++] = std::move(callback);
    else ESP_LOGE(TAG, "callback list full");
}

void add_policy(OutputPolicy* policy) {
    if (!transaction_mutex || !policy) return;
    Lock lock;
    if (policy_count < MAX_POLICIES) policies[policy_count++] = policy;
    else ESP_LOGE(TAG, "policy list full");
}
} // namespace PowerOutput

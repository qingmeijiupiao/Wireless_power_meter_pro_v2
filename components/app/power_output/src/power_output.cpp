/** Unified output transactions. Only this module may enable the main output. */
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

// One owned slot: no queued ON can unexpectedly execute after OFF. A timed-out
// waiter detaches; the worker retains the slot until the pulse has been cleaned up.
struct PendingRequest {
    bool used = false;
    bool synchronous = false;
    bool diagnostic = false;
    bool cancelled = false;
    bool completed = false;
    int64_t deadline_us = 0;
    const char* source = "unknown";
    CompletionCallback completion;
    OutputResult result = OutputResult::OK;
    ShortCircuitDetect::Result measurement{};
};
PendingRequest pending;
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

// Caller owns transaction_mutex. State is published only after GPIO success.
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

void log_result(const char* source, OutputResult result, bool state) {
    if (result == OutputResult::OK) {
        DEVICE_STATE_I(TAG, "output: source=%s result=ok state=%u", source, state ? 1U : 0U);
    } else {
        ESP_LOGW(TAG, "output: source=%s result=%s state=%u", source, result_to_string(result), state ? 1U : 0U);
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
        for (;;) {
            if (result == OutputResult::OK) {
                // Cleanup also runs for bypassed requests: a failed pulse shutdown
                // must never allow main power and test excitation to overlap.
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
            if (pending.cancelled || !initialized) result = OutputResult::FAIL_CANCELLED;
            else if (esp_timer_get_time() >= pending.deadline_us) result = OutputResult::FAIL_TIMEOUT;
            // Bypass enabled during a successful measurement also skips its
            // short-circuit verdict. Hardware cleanup errors still block ON.
            if (!diagnostic && result == OutputResult::FAIL_SHORT_CIRCUIT && protect_is_bypassed())
                result = OutputResult::OK;
            if (!diagnostic && result == OutputResult::OK) {
                result = check_policies();
                // Bypass may have been disabled while the worker was running.
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
            state = get_state();
            if (!diagnostic && (result == OutputResult::FAIL_SHORT_CIRCUIT ||
                                result == OutputResult::FAIL_SHORT_DETECT || result == OutputResult::FAIL_TIMEOUT)) {
                failure_notice = {result, measurement.voltage_mV, measurement.threshold_mV, error};
                failure_pending = true;
                published_failure = true;
                ESP_LOGW(TAG, "short preflight source=%s result=%s voltage=%u threshold=%u error=%s",
                         source, result_to_string(result), measurement.voltage_mV, measurement.threshold_mV,
                         esp_err_to_name(error));
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
        // No user callbacks under the transaction lock.
        if (published_failure) notify_event();
        log_result(source, result, state);
        if (changed) notify_change(true);
        if (completion) completion(result, state);
    }
}

OutputResult submit(OutputOperation op, const char* source, CompletionCallback completion,
                    bool synchronous, bool diagnostic) {
    source = source ? source : "unknown";
    OutputResult result = OutputResult::FAIL_NOT_INIT;
    bool changed = false;
    bool state = get_state();
    if (initialized.load()) {
        Lock lock;
        state = get_state();
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
                pending.deadline_us = esp_timer_get_time() + static_cast<int64_t>(REQUEST_TIMEOUT_MS) * 1000;
                pending.completion = std::move(completion);
                xTaskNotifyGive(worker_task);
                return OutputResult::PENDING;
            }
        }
    }
    if (changed) notify_change(false);
    log_result(source, result, state);
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
        // Worker owns storage: a timeout never leaves a pointer to the caller stack.
        pending.cancelled = true;
        pending.synchronous = false;
        if (!pending.diagnostic) {
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
    // Lifecycle initialization is called from app_main. Retain resources on
    // deinit so an in-flight worker can finish pulse cleanup safely.
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

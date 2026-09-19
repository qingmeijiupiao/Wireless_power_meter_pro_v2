/**
 * @file short_circuit_detect.cpp
 * @brief 输出端短路检测组件实现。
 */
#include "short_circuit_detect.h"

#include "HXC_NVS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>
#include <atomic>
#include <mutex>

namespace ShortCircuitDetect {
namespace {

constexpr char     TAG[]                = "ShortDetect";
constexpr char     THRESHOLD_NVS_KEY[]  = "short_th_mv";
constexpr uint32_t SETTLE_TIME_MS       = 2;

HXC::NVS_DATA<uint16_t> threshold_mV(THRESHOLD_NVS_KEY, DEFAULT_THRESHOLD_MV);
bool                    initialized = false;
std::atomic_flag test_busy = ATOMIC_FLAG_INIT;
std::mutex config_mutex;

class TestLock {
  public:
    TestLock() : acquired_(!test_busy.test_and_set()) {}
    ~TestLock() { if (acquired_) test_busy.clear(); }
    bool acquired() const { return acquired_; }
  private:
    bool acquired_;
};

/** 确保所有正常和异常退出路径都关闭短路测试激励。 */
class TestPulseGuard {
  public:
    TestPulseGuard() = default;
    ~TestPulseGuard() {
        if (enabled_) {
            const esp_err_t err = set_short_test_enabled(false);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to disable test pulse: %s", esp_err_to_name(err));
            }
        }
    }

    esp_err_t enable() {
        // Even an unsuccessful enable must attempt to restore a low level.
        enabled_ = true;
        const esp_err_t err = set_short_test_enabled(true);
        return err;
    }

    esp_err_t disable() {
        if (!enabled_) {
            return ESP_OK;
        }
        const esp_err_t err = set_short_test_enabled(false);
        if (err == ESP_OK) {
            enabled_ = false;
        }
        return err;
    }

  private:
    bool enabled_ = false;
};

} // namespace

static esp_err_t init_unlocked() {
    if (initialized) {
        return ESP_OK;
    }

    const esp_err_t err = set_short_test_enabled(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to set safe test output: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "initialized threshold=%u mV", static_cast<unsigned>(get_threshold_mV()));
    return ESP_OK;
}

esp_err_t test(Result& result, CancelCheck cancelled, void* context) {
    result = {};
    TestLock lock;
    if (!lock.acquired()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = init_unlocked();
    if (err != ESP_OK) {
        return err;
    }

    result.threshold_mV = get_threshold_mV();
    result.is_short = true;
    if (cancelled && cancelled(context)) return ESP_ERR_INVALID_STATE;
    TestPulseGuard pulse;
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(MAX_TEST_TIME_MS) * 1000;
    err = pulse.enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable test pulse: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(SETTLE_TIME_MS));

    uint8_t consecutive_good = 0;
    uint8_t consecutive_invalid = 0;
    while (esp_timer_get_time() < deadline_us) {
        if (cancelled && cancelled(context)) return ESP_ERR_INVALID_STATE;
        int voltage_mV = 0;
        err            = read_short_detect_voltage_mV(voltage_mV);
        if (err != ESP_OK || voltage_mV < 0) {
            const esp_err_t sample_error = err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
            consecutive_good = 0;
            ++consecutive_invalid;
            ESP_LOGW(TAG, "invalid ADC sample retry=%u/%u error=%s voltage=%d mV",
                     static_cast<unsigned>(consecutive_invalid),
                     static_cast<unsigned>(MAX_CONSECUTIVE_INVALID_SAMPLES),
                     esp_err_to_name(sample_error), voltage_mV);
            if (consecutive_invalid >= MAX_CONSECUTIVE_INVALID_SAMPLES) {
                ESP_LOGE(TAG, "ADC failed for %u consecutive samples: %s",
                         static_cast<unsigned>(consecutive_invalid), esp_err_to_name(sample_error));
                return sample_error;
            }
            const int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
            if (remaining_ms <= 0) break;
            vTaskDelay(pdMS_TO_TICKS(std::min<int64_t>(SAMPLE_INTERVAL_MS, remaining_ms)));
            continue;
        }
        // A delayed ADC result outside the window must not qualify an opening.
        if (esp_timer_get_time() >= deadline_us) break;
        consecutive_invalid = 0;
        result.voltage_mV = static_cast<uint16_t>(voltage_mV);
        ++result.sample_count;
        consecutive_good = voltage_mV >= result.threshold_mV ? consecutive_good + 1 : 0;
        if (consecutive_good >= REQUIRED_GOOD_SAMPLES) {
            result.is_short = false;
            break;
        }
        const int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) break;
        vTaskDelay(pdMS_TO_TICKS(std::min<int64_t>(SAMPLE_INTERVAL_MS, remaining_ms)));
    }

    err = pulse.disable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to disable test pulse: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "test result=%s voltage=%u mV threshold=%u mV samples=%u",
             result.is_short ? "SHORT" : "OPEN", static_cast<unsigned>(result.voltage_mV),
             static_cast<unsigned>(result.threshold_mV), static_cast<unsigned>(result.sample_count));
    return ESP_OK;
}

uint16_t get_threshold_mV() {
    std::lock_guard<std::mutex> lock(config_mutex);
    const uint16_t value = threshold_mV.read();
    if (value == 0 || value > MAX_THRESHOLD_MV) {
        ESP_LOGW(TAG, "invalid stored threshold=%u mV, use default=%u mV", static_cast<unsigned>(value),
                 static_cast<unsigned>(DEFAULT_THRESHOLD_MV));
        return DEFAULT_THRESHOLD_MV;
    }
    return value;
}

esp_err_t set_threshold_mV(uint16_t value) {
    std::lock_guard<std::mutex> lock(config_mutex);
    if (value == 0 || value > MAX_THRESHOLD_MV) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = threshold_mV.set(value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist threshold=%u mV: %s", static_cast<unsigned>(value), esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "threshold updated to %u mV", static_cast<unsigned>(value));
    return ESP_OK;
}

esp_err_t init() {
    TestLock lock;
    return lock.acquired() ? init_unlocked() : ESP_ERR_INVALID_STATE;
}

esp_err_t ensure_idle() {
    TestLock lock;
    if (!lock.acquired()) return ESP_ERR_INVALID_STATE;
    return set_short_test_enabled(false);
}

} // namespace ShortCircuitDetect

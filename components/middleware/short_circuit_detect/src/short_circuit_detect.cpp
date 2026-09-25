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
// 复测段至少要能采到 REQUIRED_GOOD_SAMPLES 个样本，否则不再做一次无意义的断开。
constexpr uint32_t MIN_CONFIRM_MS       = 40;

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
        // 开启失败也必须尝试恢复低电平。
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

/** 在单个时间窗口内采样，直到连续 REQUIRED_GOOD_SAMPLES 次达到阈值。 */
static esp_err_t run_probe(Result& result, uint32_t window_ms, CancelCheck cancelled, void* context, bool& passed) {
    passed = false;
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(window_ms) * 1000;
    uint8_t consecutive_good = 0;
    uint8_t consecutive_invalid = 0;
    while (esp_timer_get_time() < deadline_us) {
        if (cancelled && cancelled(context)) return ESP_ERR_INVALID_STATE;
        int voltage_mV = 0;
        const esp_err_t err = read_short_detect_voltage_mV(voltage_mV);
        if (err != ESP_OK || voltage_mV < 0) {
            const esp_err_t sample_error = err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
            // 读取失败不代表短路，不打断已累计的达标样本；只有有效低电压才清零。
            ++consecutive_invalid;
            ++result.invalid_count;
            ESP_LOGD(TAG, "invalid ADC sample retry=%u/%u error=%s voltage=%d mV",
                     static_cast<unsigned>(consecutive_invalid),
                     static_cast<unsigned>(MAX_CONSECUTIVE_INVALID_SAMPLES),
                     esp_err_to_name(sample_error), voltage_mV);
            if (consecutive_invalid >= MAX_CONSECUTIVE_INVALID_SAMPLES) {
                // 最终故障及重试统计由输出仲裁层统一记录。
                return sample_error;
            }
            const int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
            if (remaining_ms <= 0) break;
            vTaskDelay(pdMS_TO_TICKS(std::min<int64_t>(SAMPLE_INTERVAL_MS, remaining_ms)));
            continue;
        }
        // 超过采样窗口才返回的 ADC 样本不能用于判定通过。
        if (esp_timer_get_time() >= deadline_us) break;
        consecutive_invalid = 0;
        result.voltage_mV = static_cast<uint16_t>(voltage_mV);
        if (result.voltage_mV < result.min_voltage_mV) result.min_voltage_mV = result.voltage_mV;
        ++result.sample_count;
        consecutive_good = voltage_mV >= result.threshold_mV ? consecutive_good + 1 : 0;
        if (consecutive_good >= REQUIRED_GOOD_SAMPLES) {
            passed = true;
            break;
        }
        const int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) break;
        vTaskDelay(pdMS_TO_TICKS(std::min<int64_t>(SAMPLE_INTERVAL_MS, remaining_ms)));
    }
    return ESP_OK;
}

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
    // 声明在激励守卫之前，退出时连同清理耗时一并统计。
    struct TimingGuard {
        Result& result;
        int64_t started_us = esp_timer_get_time();
        ~TimingGuard() { result.duration_ms = static_cast<uint32_t>((esp_timer_get_time() - started_us) / 1000); }
    } timing{result};
    TestLock lock;
    if (!lock.acquired()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = init_unlocked();
    if (err != ESP_OK) {
        return err;
    }

    result.threshold_mV   = get_threshold_mV();
    result.min_voltage_mV = UINT16_MAX;
    result.is_short       = true;
    if (cancelled && cancelled(context)) return ESP_ERR_INVALID_STATE;

    TestPulseGuard pulse;
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(MAX_TEST_TIME_MS) * 1000;
    bool passed = false;

    err = pulse.enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable test pulse: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(SETTLE_TIME_MS));
    err = run_probe(result, FIRST_PROBE_MS, cancelled, context, passed);
    if (err != ESP_OK) return err;

    // 可疑（仍为低电平）时反复“断开激励 → 复测”，给低启动电压/低阻负载退出低阻态
    // 的机会，直到用完总预算；任一复测段连续三次达标即判开路。
    while (!passed) {
        const int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms <= static_cast<int64_t>(RELEASE_GAP_MS) + static_cast<int64_t>(MIN_CONFIRM_MS)) break;
        err = pulse.disable();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to disable test pulse: %s", esp_err_to_name(err));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(RELEASE_GAP_MS));
        err = pulse.enable();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to enable confirmation pulse: %s", esp_err_to_name(err));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(SETTLE_TIME_MS));
        const int64_t window_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (window_ms <= 0) break;
        const uint32_t probe_window = std::min<uint32_t>(CONFIRM_WINDOW_MS, static_cast<uint32_t>(window_ms));
        err = run_probe(result, probe_window, cancelled, context, passed);
        if (err != ESP_OK) return err;
    }

    result.is_short = !passed;
    if (result.sample_count == 0) result.min_voltage_mV = 0;

    err = pulse.disable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to disable test pulse: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "test result=%s voltage=%u min=%u threshold=%u mV samples=%u",
             result.is_short ? "SHORT" : "OPEN", static_cast<unsigned>(result.voltage_mV),
             static_cast<unsigned>(result.min_voltage_mV), static_cast<unsigned>(result.threshold_mV),
             static_cast<unsigned>(result.sample_count));
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

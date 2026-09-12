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

namespace ShortCircuitDetect {
namespace {

constexpr char     TAG[]                = "ShortDetect";
constexpr char     THRESHOLD_NVS_KEY[]  = "short_th_mv";
constexpr uint8_t  SAMPLE_COUNT         = 4;
constexpr uint32_t SETTLE_TIME_MS       = 2;
constexpr uint32_t SAMPLE_INTERVAL_MS   = 1;

HXC::NVS_DATA<uint16_t> threshold_mV(THRESHOLD_NVS_KEY, DEFAULT_THRESHOLD_MV);
bool                    initialized = false;

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
        const esp_err_t err = set_short_test_enabled(true);
        enabled_            = err == ESP_OK;
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

esp_err_t init() {
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

esp_err_t test(Result& result) {
    result = {};
    esp_err_t err = init();
    if (err != ESP_OK) {
        return err;
    }

    TestPulseGuard pulse;
    err = pulse.enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable test pulse: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(SETTLE_TIME_MS));

    uint32_t voltage_sum_mV = 0;
    for (uint8_t i = 0; i < SAMPLE_COUNT; ++i) {
        int voltage_mV = 0;
        err            = read_short_detect_voltage_mV(voltage_mV);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed at sample %u: %s", static_cast<unsigned>(i), esp_err_to_name(err));
            return err;
        }
        if (voltage_mV < 0) {
            ESP_LOGE(TAG, "ADC returned invalid voltage: %d mV", voltage_mV);
            return ESP_ERR_INVALID_RESPONSE;
        }
        voltage_sum_mV += static_cast<uint32_t>(voltage_mV);
        if (i + 1 < SAMPLE_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
        }
    }

    err = pulse.disable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to disable test pulse: %s", esp_err_to_name(err));
        return err;
    }

    result.voltage_mV   = static_cast<uint16_t>(voltage_sum_mV / SAMPLE_COUNT);
    result.threshold_mV = get_threshold_mV();
    result.sample_count = SAMPLE_COUNT;
    result.is_short     = result.voltage_mV < result.threshold_mV;

    ESP_LOGI(TAG, "test result=%s voltage=%u mV threshold=%u mV samples=%u",
             result.is_short ? "SHORT" : "OPEN", static_cast<unsigned>(result.voltage_mV),
             static_cast<unsigned>(result.threshold_mV), static_cast<unsigned>(result.sample_count));
    return ESP_OK;
}

uint16_t get_threshold_mV() {
    const uint16_t value = threshold_mV.read();
    if (value == 0 || value > MAX_THRESHOLD_MV) {
        ESP_LOGW(TAG, "invalid stored threshold=%u mV, use default=%u mV", static_cast<unsigned>(value),
                 static_cast<unsigned>(DEFAULT_THRESHOLD_MV));
        return DEFAULT_THRESHOLD_MV;
    }
    return value;
}

esp_err_t set_threshold_mV(uint16_t value) {
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

} // namespace ShortCircuitDetect

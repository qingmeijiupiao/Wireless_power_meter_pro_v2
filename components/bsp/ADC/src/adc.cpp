/*
 * @Description:
 * @Author: qingmeijiupiao
 * @version:
 * @Date: 2026-04-25 01:28:58
 * @LastEditTime: 2026-05-18 01:22:28
 */
#include "adc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h" // 包含校准方案特定函数
#include <mutex>

adc_oneshot_unit_handle_t adc_t::adc1_unit_handle = nullptr;

namespace {
// 多个 adc_t 实例共享同一 ADC1 单元，可能被不同任务并发读取。
// adc_oneshot_read 内部用 try-lock 获取单元锁，拿不到会立即返回 ESP_ERR_TIMEOUT，
// 因此这里用阻塞互斥把单元访问串行化，避免并发读互相打断。
std::mutex adc_unit_mutex;
} // namespace

adc_oneshot_unit_init_cfg_t init_config1 = {
    .unit_id  = ADC_UNIT_1,
    .clk_src  = ADC_DIGI_CLK_SRC_DEFAULT,
    .ulp_mode = ADC_ULP_MODE_DISABLE,
};

adc_oneshot_chan_cfg_t config = {
    .atten    = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
};

adc_cali_curve_fitting_config_t cali_config = {
    .unit_id  = ADC_UNIT_1,
    .chan     = ADC_CHANNEL_0,
    .atten    = ADC_ATTEN_DB_12,      // 必须与通道配置的衰减一致
    .bitwidth = ADC_BITWIDTH_DEFAULT, // 必须与通道配置的位宽一致
};

esp_err_t adc_t::init() {
    esp_err_t ret = ESP_OK;
    if (adc1_unit_handle == NULL) {
        ret = adc_oneshot_new_unit(&init_config1, &adc1_unit_handle);
        if (ret != ESP_OK) {
            ESP_LOGE("ADC", "adc_oneshot_new_unit failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGD("ADC", "adc_oneshot_new_unit success");
    }
    ret = adc_oneshot_config_channel(adc1_unit_handle, adc_channel, &config);
    if (ret != ESP_OK) {
        ESP_LOGE("ADC", "adc_oneshot_config_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }
    cali_config.chan = adc_channel;
    ret              = adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle);
    if (ret != ESP_OK) {
        ESP_LOGE("ADC", "adc calibration init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t adc_t::read_raw(int& raw) {
    esp_err_t ret = ESP_OK;
    if (adc1_unit_handle == NULL) {
        ret = ESP_ERR_INVALID_STATE;
        return ret;
    }
    std::lock_guard<std::mutex> lock(adc_unit_mutex);
    int adc_value = 0;
    ret           = adc_oneshot_read(adc1_unit_handle, adc_channel, &adc_value);
    if (ret != ESP_OK) {
        return ret;
    }
    raw = adc_value;
    return ret;
}

esp_err_t adc_t::read_voltage_mV(int& voltage) {
    if (cali_handle == nullptr) {
        ESP_LOGE("ADC", "calibration handle unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;

    int adc_value = 0;
    ret           = read_raw(adc_value);
    if (ret != ESP_OK) {
        return ret;
    }

    return adc_cali_raw_to_voltage(cali_handle, adc_value, &voltage);
}

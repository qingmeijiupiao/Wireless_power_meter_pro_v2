/*
 * @Description:
 * @Author: qingmeijiupiao
 * @version:
 * @Date: 2026-04-25 00:50:00
 * @LastEditTime: 2026-04-29 00:34:50
 */
#include "hardware.h"
#include "adc.h"
#include <cstdlib>

adc_t                 hardware_adc(hardware_adc_channel);
adc_t                 short_detect_adc(ADC_CHANNEL_5);
uint8_t               hardware_version = 255;
const hardware_config version_0        = {
           .TFT_SCL              = GPIO_NUM_23,
           .TFT_SDA              = GPIO_NUM_2,
           .TFT_RST              = GPIO_NUM_21,
           .TFT_RS               = GPIO_NUM_22,
           .TFT_CS               = GPIO_NUM_8,
           .TFT_BLK              = GPIO_NUM_1,
           .TFT_BLK_ACTIVE_STATE = true,
           .temperature_channel  = ADC_CHANNEL_3,
           .short_detect_channel = ADC_CHANNEL_5,
           .CAN_TX               = GPIO_NUM_15,
           .CAN_RX               = GPIO_NUM_14,
           .CAN_RESISTOR_ENABLE  = GPIO_NUM_18,
           .INA228_SDA            = GPIO_NUM_6,
           .INA228_SCL            = GPIO_NUM_7,
           .INA228_ALERT          = GPIO_NUM_4,
           .OUTPUT_CTRL          = GPIO_NUM_19,
           .SHORT_TEST_ENABLE    = GPIO_NUM_20,
           .MAIN_BUTTON          = GPIO_NUM_16,
           .SIDE_BUTTON          = GPIO_NUM_17,
           .PREVIOUS_BUTTON      = GPIO_NUM_9,
};

#define TAG "hardware"
esp_err_t hardware_config_init() {
    constexpr int VERSION_STEP_MV = 330;
    constexpr int HALF_STEP_MV = VERSION_STEP_MV / 2;
    hardware_version = 255;
    esp_err_t ret = ESP_OK;
    gpio_config_t short_test_config = {};
    short_test_config.pin_bit_mask = 1ULL << version_0.SHORT_TEST_ENABLE;
    short_test_config.mode = GPIO_MODE_OUTPUT;
    ret = gpio_config(&short_test_config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = gpio_set_level(version_0.SHORT_TEST_ENABLE, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    ret           = hardware_adc.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hardware version ADC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    int voltage_mV = 0;
    int sum       = 0;
    int err_count = 0;
    for (int i = 0; i < 10; i++) {
        ret = hardware_adc.read_voltage_mV(voltage_mV);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
            return ret;
        }
        if (voltage_mV < 0 || voltage_mV > 3300) {
            ESP_LOGE(TAG, "hardware version voltage out of range: %d mV", voltage_mV);
            return ESP_ERR_INVALID_STATE;
        }
        if ((i > 0) && (std::abs(voltage_mV - (sum / i)) > HALF_STEP_MV)) {
            err_count++;
            i--;
            ESP_LOGW(TAG, "ADC value is invalid, err_count: %d", err_count);
            if (err_count > 5) {
                ESP_LOGE(TAG, "ADC value is invalid, err_count: %d", err_count);
                return ESP_ERR_INVALID_STATE; // 多次偏离均值半档以上，版本识别不可靠。
            }
            continue;
        }
        ESP_LOGD(TAG, "hardware version voltage: %d mV", voltage_mV);
        sum += voltage_mV;
    }
    voltage_mV = sum / 10;

    // GPIO0 每330mV一档，0mV为版本0；完成整数运算后再窄化，避免高位被提前截断。
    hardware_version = static_cast<uint8_t>((voltage_mV + HALF_STEP_MV) / VERSION_STEP_MV);
    ESP_LOGI(TAG, "Hardware version: %d, voltage: %d mV", hardware_version, voltage_mV);

    ret = short_detect_adc.init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "short detect ADC unavailable: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

uint8_t get_hardware_version() {
    return hardware_version;
}

const hardware_config& get_hardware_config() {
    switch (get_hardware_version()) {
    case 0:
        return version_0;
    default:
        ESP_LOGW(TAG, "Unknown hardware version: %d, use default config", get_hardware_version());
        return version_0;
    }
}

esp_err_t read_short_detect_raw(int& raw) {
    return short_detect_adc.read_raw(raw);
}

esp_err_t read_short_detect_voltage_mV(int& voltage_mV) {
    return short_detect_adc.read_voltage_mV(voltage_mV);
}

esp_err_t set_short_test_enabled(bool enabled) {
    return gpio_set_level(get_hardware_config().SHORT_TEST_ENABLE, enabled ? 1 : 0);
}

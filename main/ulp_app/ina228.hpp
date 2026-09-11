#ifndef INA228_HPP
#define INA228_HPP

#include <stdint.h>
#include <stddef.h>
#include "ulp_lp_core_i2c.h"

namespace INA228 {

constexpr uint8_t  I2C_ADDR        = 0x40;
constexpr int      I2C_TIMEOUT     = 10 * 20000;
constexpr uint16_t MANUFACTURER_ID = 0x5449;
constexpr uint16_t DEVICE_ID_MASK  = 0xFFF0;
constexpr uint16_t DEVICE_ID       = 0x2280;

enum Register : uint8_t {
    CONFIG          = 0x00,
    ADC_CONFIG      = 0x01,
    SHUNT_CAL       = 0x02,
    SHUNT_TEMPCO    = 0x03,
    VSHUNT          = 0x04,
    VBUS            = 0x05,
    DIETEMP         = 0x06,
    CURRENT         = 0x07,
    POWER           = 0x08,
    ENERGY          = 0x09,
    CHARGE          = 0x0A,
    DIAG_ALRT       = 0x0B,
    MANUFACTURER    = 0x3E,
    DEVICE          = 0x3F,
};

inline esp_err_t read(Register reg, uint8_t* data, size_t size) {
    uint8_t reg_byte = static_cast<uint8_t>(reg);
    return lp_core_i2c_master_write_read_device(LP_I2C_NUM_0, I2C_ADDR, &reg_byte, 1, data, size, I2C_TIMEOUT);
}

inline esp_err_t read16(Register reg, uint16_t* value) {
    uint8_t data[2] = {};
    const esp_err_t ret = read(reg, data, sizeof(data));
    if (ret == ESP_OK && value != nullptr) {
        *value = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    }
    return ret;
}

inline esp_err_t read24(Register reg, uint32_t* value) {
    uint8_t data[3] = {};
    const esp_err_t ret = read(reg, data, sizeof(data));
    if (ret == ESP_OK && value != nullptr) {
        *value = (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) | data[2];
    }
    return ret;
}

inline esp_err_t write16(Register reg, uint16_t value) {
    uint8_t data[3] = {static_cast<uint8_t>(reg), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    uint8_t placeholder = 0;
    return lp_core_i2c_master_write_read_device(LP_I2C_NUM_0, I2C_ADDR, data, sizeof(data), &placeholder, 1,
                                                 I2C_TIMEOUT);
}

inline int32_t decode_signed20(uint32_t register_value) {
    uint32_t value = (register_value >> 4) & 0xFFFFF;
    if ((value & 0x80000U) != 0) {
        value |= 0xFFF00000U;
    }
    return static_cast<int32_t>(value);
}

inline uint32_t decode_unsigned20(uint32_t register_value) {
    return (register_value >> 4) & 0xFFFFF;
}

inline esp_err_t reset() {
    return write16(CONFIG, 1U << 15);
}

inline esp_err_t configure() {
    // ADCRANGE=0 (±163.84mV), continuous VBUS/VSHUNT/TEMP, 1052us, 64-sample averaging.
    const esp_err_t config_ret = write16(CONFIG, 0x0000);
    if (config_ret != ESP_OK) {
        return config_ret;
    }
    constexpr uint16_t adc_config = (0xFU << 12) | (5U << 9) | (5U << 6) | (5U << 3) | 3U;
    return write16(ADC_CONFIG, adc_config);
}

} // namespace INA228

#endif

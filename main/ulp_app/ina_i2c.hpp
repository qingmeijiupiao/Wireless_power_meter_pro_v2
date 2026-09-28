#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ulp_lp_core_i2c.h"

namespace InaI2c {
// LP-local address selected only after matching the manufacturer and device IDs.
static uint8_t active_address = 0x40;
constexpr int I2C_TIMEOUT = 10 * 20000;
inline esp_err_t read(uint8_t reg, uint8_t* data, size_t size) {
    uint8_t reg_byte = static_cast<uint8_t>(reg);
    return lp_core_i2c_master_write_read_device(LP_I2C_NUM_0, active_address, &reg_byte, 1, data, size, I2C_TIMEOUT);
}

inline esp_err_t read16(uint8_t reg, uint16_t* value) {
    uint8_t data[2] = {};
    const esp_err_t ret = read(reg, data, sizeof(data));
    if (ret == ESP_OK && value != nullptr) {
        *value = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    }
    return ret;
}

inline esp_err_t read24(uint8_t reg, uint32_t* value) {
    uint8_t data[3] = {};
    const esp_err_t ret = read(reg, data, sizeof(data));
    if (ret == ESP_OK && value != nullptr) {
        *value = (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) | data[2];
    }
    return ret;
}

inline esp_err_t write16(uint8_t reg, uint16_t value) {
    uint8_t data[3] = {static_cast<uint8_t>(reg), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    return lp_core_i2c_master_write_to_device(LP_I2C_NUM_0, active_address, data, sizeof(data), I2C_TIMEOUT);
}

} // namespace InaI2c

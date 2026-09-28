#pragma once
#include "ina_i2c.hpp"

namespace INA226 {
constexpr uint16_t MANUFACTURER_ID = 0x5449;
constexpr uint16_t DEVICE_ID = 0x2260;
constexpr uint16_t DEVICE_ID_MASK = 0xFFF0;
enum Register : uint8_t {
    CONFIG = 0x00, VSHUNT = 0x01, VBUS = 0x02,
    MASK_ENABLE = 0x06, MANUFACTURER = 0xFE, DEVICE = 0xFF,
};
// Shared big-endian I2C transport uses the address selected by identity detection.
inline esp_err_t read16(Register reg, uint16_t* value) {
    return InaI2c::read16(reg, value);
}
inline esp_err_t reset() { return InaI2c::write16(CONFIG, 0x8000); }
inline esp_err_t configure() {
    // Reserved bit 14 retains its reset value; 64 averages, 1.1ms per channel,
    // continuous shunt + bus conversion (140.8ms per complete sample).
    return InaI2c::write16(CONFIG, 0x4000 | (3U << 9) | (4U << 6) | (4U << 3) | 7U);
}
} // namespace INA226

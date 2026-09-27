#ifndef INA228_HPP
#define INA228_HPP

#include <stdint.h>
#include <stddef.h>
#include "ina_i2c.hpp"

namespace INA228 {

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

inline esp_err_t read16(Register reg, uint16_t* value) { return InaI2c::read16(reg, value); }
inline esp_err_t read24(Register reg, uint32_t* value) { return InaI2c::read24(reg, value); }
inline esp_err_t write16(Register reg, uint16_t value) { return InaI2c::write16(reg, value); }

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

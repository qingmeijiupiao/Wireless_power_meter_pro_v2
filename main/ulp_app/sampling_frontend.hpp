#pragma once
#include "ina226.hpp"
#include "ina228.hpp"
#include "../../components/app/current_calibration/include/VoltageCalib.h"

namespace Sampling {
struct Sample {
    uint32_t voltage_uv;
    uint16_t voltage_raw;
    int16_t shunt_raw;
};

inline SamplingFrontend detect() {
    uint16_t manufacturer = 0, device = 0;
    if (INA228::read16(INA228::MANUFACTURER, &manufacturer) == ESP_OK &&
        manufacturer == INA228::MANUFACTURER_ID && INA228::read16(INA228::DEVICE, &device) == ESP_OK &&
        (device & INA228::DEVICE_ID_MASK) == INA228::DEVICE_ID) {
        return SamplingFrontend::INA228;
    }
    if (INA226::read16(INA226::MANUFACTURER, &manufacturer) == ESP_OK &&
        manufacturer == INA226::MANUFACTURER_ID && INA226::read16(INA226::DEVICE, &device) == ESP_OK &&
        (device & INA226::DEVICE_ID_MASK) == INA226::DEVICE_ID) {
        return SamplingFrontend::INA226;
    }
    return SamplingFrontend::Unknown;
}

inline esp_err_t reset(SamplingFrontend type) {
    return type == SamplingFrontend::INA226 ? INA226::reset() : INA228::reset();
}
inline esp_err_t configure(SamplingFrontend type) {
    return type == SamplingFrontend::INA226 ? INA226::configure() : INA228::configure();
}
inline bool read(SamplingFrontend type, Sample& result) {
    uint16_t status = 0;
    Sample sample = {};
    if (type == SamplingFrontend::INA226) {
        uint16_t shunt = 0;
        if (INA226::read16(INA226::MASK_ENABLE, &status) != ESP_OK || !(status & (1U << 3)) ||
            INA226::read16(INA226::VBUS, &sample.voltage_raw) != ESP_OK ||
            INA226::read16(INA226::VSHUNT, &shunt) != ESP_OK) return false;
        sample.voltage_uv = static_cast<uint32_t>(sample.voltage_raw) * 1250U;
        sample.shunt_raw = static_cast<int16_t>(shunt);
    } else if (type == SamplingFrontend::INA228) {
        uint32_t bus = 0, shunt = 0;
        if (INA228::read16(INA228::DIAG_ALRT, &status) != ESP_OK || !(status & (1U << 1)) ||
            INA228::read24(INA228::VBUS, &bus) != ESP_OK ||
            INA228::read24(INA228::VSHUNT, &shunt) != ESP_OK) return false;
        sample.voltage_uv = static_cast<uint32_t>(static_cast<uint64_t>(INA228::decode_unsigned20(bus)) * 3125U / 16U);
        const uint32_t bus_raw = sample.voltage_uv / 1250U;
        sample.voltage_raw = static_cast<uint16_t>(bus_raw > UINT16_MAX ? UINT16_MAX : bus_raw);
        // Preserve the existing 2.5uV calibration domain; saturate instead of wrapping.
        const int32_t raw = INA228::decode_signed20(shunt) / 8;
        sample.shunt_raw = static_cast<int16_t>(raw > INT16_MAX ? INT16_MAX : raw < INT16_MIN ? INT16_MIN : raw);
    } else {
        return false;
    }
    result = sample; // Never expose a partial I2C read as a new sample.
    return true;
}
} // namespace Sampling

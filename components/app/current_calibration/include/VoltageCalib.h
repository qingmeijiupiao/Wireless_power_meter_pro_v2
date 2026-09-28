#pragma once
#include <stdint.h>

// Shared by HP and LP; independent of NVS and the ESP-IDF runtime.
enum class SamplingFrontend : uint32_t { Unknown = 0, INA226 = 226, INA228 = 228 };

namespace VoltageCalib {
constexpr uint32_t SCALE = 1000000;
constexpr uint32_t DEFAULT_INA226 = 2 * SCALE;
constexpr uint32_t MIN_K = SCALE / 2;
constexpr uint32_t MAX_K = 4 * SCALE;

constexpr bool valid(uint32_t k) { return k >= MIN_K && k <= MAX_K; }
constexpr uint32_t effective_k(SamplingFrontend frontend, uint32_t stored) {
    return frontend == SamplingFrontend::INA226 ? (valid(stored) ? stored : DEFAULT_INA226) : SCALE;
}
inline uint32_t apply(uint32_t voltage_uv, uint32_t k) {
    return static_cast<uint32_t>((static_cast<uint64_t>(voltage_uv) * k + SCALE / 2) / SCALE);
}
struct Runtime {
    SamplingFrontend frontend = SamplingFrontend::Unknown;
    uint32_t active_k = SCALE;
    uint32_t uncalibrated_uv = 0;
    bool available = false;
    uint32_t lp_state = 0;
    uint32_t address_mask = 0;
    uint32_t i2c_address = 0;
    int32_t identity_error[4] = {};
    uint16_t identity_value[4] = {};
};
Runtime get_runtime();
} // namespace VoltageCalib

#pragma once
#include <cstdint>
#include <cstddef>
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int LP_I2C_NUM_0 = 0;
esp_err_t lp_core_i2c_master_write_read_device(int, uint16_t, const uint8_t*, size_t, uint8_t*, size_t, int32_t);
esp_err_t lp_core_i2c_master_write_to_device(int, uint16_t, const uint8_t*, size_t, int32_t);

#include <cassert>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>
#define main lp_firmware_main
#include "../../main/ulp_app/ulp_main.cpp"
#undef main

uint32_t simulated_cycles = 0;
static std::map<uint8_t, std::pair<uint32_t, size_t>> registers;
static std::vector<std::pair<uint8_t, uint16_t>> writes;
static int failed_register = -1;
static int reset_failures = 0;

esp_err_t lp_core_i2c_master_write_read_device(int, uint16_t address, const uint8_t* tx, size_t tx_size,
                                              uint8_t* rx, size_t rx_size, int32_t) {
    assert(address == 0x40 && tx_size == 1);
    const auto it = registers.find(tx[0]);
    if (tx[0] == failed_register || it == registers.end()) return -1;
    assert(rx_size == it->second.second); // Catch accidental 16/24-bit register confusion.
    for (size_t i = 0; i < rx_size; ++i) rx[i] = it->second.first >> (8 * (rx_size - 1 - i));
    return ESP_OK;
}
esp_err_t lp_core_i2c_master_write_to_device(int, uint16_t address, const uint8_t* tx, size_t size, int32_t) {
    assert(address == 0x40 && size == 3);
    const uint16_t value = (uint16_t(tx[1]) << 8) | tx[2];
    writes.emplace_back(tx[0], value);
    if (value == 0x8000 && reset_failures > 0) { --reset_failures; return -1; }
    return ESP_OK;
}
static void chip226(uint16_t revision = 0x2260) {
    registers = {{0xFE, {0x5449, 2}}, {0xFF, {revision, 2}}, {6, {8, 2}},
                 {2, {9600, 2}}, {1, {800, 2}}}; // 12V at pin, 2mV shunt.
}
static void chip228() {
    registers = {{0x3E, {0x5449, 2}}, {0x3F, {0x2281, 2}}, {0x0B, {2, 2}},
                 {5, {122880U << 4, 3}}, {4, {6400U << 4, 3}}}; // 24V, 2mV shunt.
}
int main() {
    using F = SamplingFrontend;
    assert(Sampling::detect() == F::Unknown);
    chip226();
    assert(Sampling::detect() == F::INA226);
    chip226(0x2261);
    assert(Sampling::detect() == F::INA226);
    registers[0xFE].first = 0x1234;
    assert(Sampling::detect() == F::Unknown);
    chip226(0x2270);
    assert(Sampling::detect() == F::Unknown);
    chip228();
    assert(Sampling::detect() == F::INA228);
    failed_register = 0x3F;
    assert(Sampling::detect() == F::Unknown);
    failed_register = -1;

    assert(VoltageCalib::effective_k(F::INA226, 0) == 2000000);
    assert(VoltageCalib::effective_k(F::INA226, 4000001) == 2000000);
    assert(VoltageCalib::effective_k(F::INA228, 2100000) == 1000000);
    assert(VoltageCalib::apply(12000000, 2001000) == 24012000);
    assert(VoltageCalib::apply(1250, 500000) == 625);

    chip226();
    current_calib_params.current_base_K = 1250;
    Board_temperature = 3500;
    ina226_voltage_k = 2000000;
    load_current_calib_params();
    reset_failures = 1;
    assert(ulp_frontend_init()); // Retries a failed reset.
    assert(sampling_frontend == 226 && voltage_uv == 24000000 && current_uA == 1000000);
    assert(voltage_register_raw == 9600 && shunt_register_raw == 800);
    assert(writes.back().second == 0x4727);

    const uint32_t old_voltage = voltage_uv;
    registers[2].first = 10000;
    failed_register = 1; // Half-read must not publish the new bus voltage.
    frontend_run();
    assert(voltage_uv == old_voltage);
    failed_register = -1;
    registers[6].first = 0; // Conversion not ready.
    frontend_run();
    assert(voltage_uv == old_voltage);
    registers[6].first = 8;
    registers[1].first = 0xFCE0; // -800 signed LSB.
    frontend_run();
    assert(current_uA == -1000000);

    ina226_voltage_k = 2100000;
    ulp_state_p.ulp_state_bits.ulp_reload_calib_params = true;
    check_reload_current_calib_params();
    frontend_run();
    assert(voltage_uv == 26250000 && active_voltage_k == 2100000);
    assert(!ulp_state_p.ulp_state_bits.ulp_reload_calib_params);

    // A failed old-chip read triggers timeout recovery and fresh identification.
    chip228();
    simulated_cycles += 1100 * 20000;
    timer_run();
    frontend_run();
    assert(sampling_frontend == 228 && active_voltage_k == 1000000);
    assert(voltage_uv == 24000000 && current_uA == 1000000 && shunt_register_raw == 800);
    assert(!ulp_state_p.ulp_state_bits.ulp_i2c_init_err);
    assert(writes.back().second == 0xFB6B);

    registers[4].first = 0x7FFFF0;
    frontend_run();
    assert(shunt_register_raw == INT16_MAX && current_uA > 0);
    for (int i = 0; i < 6; ++i) {
        current_calib_params.points[i].register_value = (i + 1) * 100;
        current_calib_params.points[i].offset_current_100uA = i + 1;
    }
    load_current_calib_params();
    registers[4].first = 0x800000;
    frontend_run();
    assert(shunt_register_raw == INT16_MIN && current_uA == -40960600);
    registers[5].first = 0xFFFFF0;
    frontend_run();
    assert(voltage_register_raw == UINT16_MAX);

    // Zero volts is a valid first conversion, not a reason to reset forever.
    chip226();
    registers[2].first = 0;
    assert(ulp_frontend_init());
    assert(voltage_uv == 0 && ulp_state_p.ulp_state_bits.ulp_ina228_init_ok);
    puts("PASS: identification, configuration, calibration, signed bounds, partial reads, recovery, zero-V startup");
}

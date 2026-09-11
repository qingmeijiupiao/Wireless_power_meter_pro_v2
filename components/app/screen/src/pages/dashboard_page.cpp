/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 实时测量主页实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-24
 */
#include "pages/dashboard_page.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>

#include "blackbox.h"
#include "diagnostic_log.h"
#include "DENGB12.h"
#include "DENGB16.h"
#include "DENGB20.h"
#include "current_calibration.h"
#include "energy_meter.h"
#include "espnow_link.h"
#include "espnow_service.h"
#include "esp_log.h"
#include "ErrorRectangle.h"
#include "WarningRectangle.h"
#include "blackbox_service.h"
#include "can_callback.h"
#include "can_resistor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "HXC_NVS.h"
#include "meter_a_logo.h"
#include "meter_circle_green.h"
#include "meter_circle_red.h"
#include "meter_v_logo.h"
#include "meter_w_logo.h"
#include "settings_logo.h"
#include "st7789.h"
#include "ota_service.h"
#include "ui_close.h"
#include "ui_open.h"
#include "ui_static.h"
#include "wifi_manager.h"
#include "wifi_service.h"
#include "ah_logo.h"
#include "wh_logo.h"

namespace SCREEN {
namespace {

constexpr char TAG[] = "ScreenPages";

/**
 * @brief 将秒数格式化为时分秒文本。
 * @param line 输出缓冲区。
 * @param line_size 输出缓冲区大小。
 * @param prefix 可选前缀，例如 "S:"；传入 nullptr 表示无前缀。
 * @param total_seconds 总秒数。
 */
void format_duration(char* line, size_t line_size, const char* prefix, uint64_t total_seconds) {
    snprintf(line, line_size, "%s%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, prefix == nullptr ? "" : prefix,
             static_cast<uint64_t>(total_seconds / 3600),
             static_cast<uint64_t>((total_seconds / 60) % 60),
             static_cast<uint64_t>(total_seconds % 60));
}

} // namespace

PageId DashboardPage::id() const {
    return PageId::Dashboard;
}

/** @brief 返回主页标题。 */
const char* DashboardPage::title() const {
    return "Main";
}

/** @brief 返回主页刷新周期。 */
uint32_t DashboardPage::refresh_interval_ms() const {
    return 1000 / 30;
}

/**
 * @brief 绘制主页实时测量值、运行时间、输出状态和保护状态。
 * @param mode 页面渲染模式，主页始终执行整屏重绘。
 */
void DashboardPage::render(RenderMode mode) {
    (void)mode;
    char        temp_str[16];
    const auto  global_state       = get_global_state();
    const auto& global_state_flags = global_state.flags;
    const auto& protect_states     = global_state.protect_states.states_bit;
    const float voltage            = global_state.voltage_mV / 1000.0f;
    const float current            = std::abs(global_state.current_uA / 1000000.0f);

    auto draw_static_layout = []() {
        ST7789::fill_screen(ST7789::BLACK);
        ST7789::draw_image(4, 4, STATIC_WIDTH, STATIC_HEIGHT, static_data);
        ST7789::fill_rect(106, 0, 2, 80, ST7789::YELLOW);
        ST7789::fill_rect(108, 13, 52, 2, ST7789::YELLOW);
    };

    auto draw_measurements = [&]() {
        snprintf(temp_str, sizeof(temp_str), "%.3fV", voltage);
        ST7789::draw_string(28, 4, temp_str, ST7789::color_t(0xef2a2a), ST7789::BLACK, DENGB20);
        snprintf(temp_str, sizeof(temp_str), "%.3fA", current);
        ST7789::draw_string(28, 27, temp_str, ST7789::color_t(0x1ef851), ST7789::BLACK, DENGB20);
        snprintf(temp_str, sizeof(temp_str), "%.3fW", current * voltage);
        ST7789::draw_string(28, 49, temp_str, ST7789::color_t(0x003ED0), ST7789::BLACK, DENGB16);

        const float temperature = global_state.board_temperature / 100.0f;
        if (temperature >= 100.0f || temperature < 0.0f) {
            snprintf(temp_str, sizeof(temp_str), "%dC", static_cast<int>(temperature));
        } else {
            snprintf(temp_str, sizeof(temp_str), "%.1fC", temperature);
        }
        ST7789::draw_string(28, 69, temp_str, ST7789::color_t(0xb3261e), ST7789::BLACK, DENGB12);
    };

    auto draw_uptime = [&]() {
        const uint32_t total_seconds = (xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000;
        format_duration(temp_str, sizeof(temp_str), nullptr, total_seconds);
        ST7789::draw_string(111, 2, temp_str, ST7789::WHITE, ST7789::BLACK, DENGB12);
    };

    auto draw_output_state = [&]() {
        const bool enabled = global_state_flags.output_enabled;
        ST7789::draw_image(62, 66, enabled ? OPEN_WIDTH : CLOSE_WIDTH, enabled ? OPEN_HEIGHT : CLOSE_HEIGHT,
                           enabled ? open_data : close_data);
    };

    auto draw_protect_states = [&]() {
        draw_protect_tag(113, 18, "OTP", protect_states.temperature_protect_state);
        ProtectState_t voltage_state = protect_states.high_voltage_protect_state;
        const char*    voltage_text  = "OVP";
        if (voltage_state == PROTECT_STATE_NORMAL) {
            voltage_state = protect_states.low_voltage_protect_state;
            voltage_text  = "UVP";
        }
        draw_protect_tag(113, 39, voltage_text, voltage_state);
        draw_protect_tag(113, 60, "OCP", protect_states.current_protect_state);
    };

    draw_static_layout();
    draw_measurements();
    draw_uptime();
    draw_output_state();
    draw_protect_states();
}

/**
 * @brief 绘制主页右侧的单个保护状态标签。
 * @param x 标签左上角 X 坐标。
 * @param y 标签左上角 Y 坐标。
 * @param text 标签文本。
 * @param state 保护状态。
 */
void DashboardPage::draw_protect_tag(uint16_t x, uint16_t y, const char* text, ProtectState_t state) {
    if (state == PROTECT_STATE_NORMAL) {
        return;
    }

    ST7789::color_t warning_background_color;
    warning_background_color.set_color_raw(0xFE60);
    ST7789::color_t error_background_color;
    error_background_color.set_color_raw(0xB123);

    if (state == PROTECT_STATE_PROTECT) {
        ST7789::draw_image(x, y, ERRORRECTANGLE_WIDTH, ERRORRECTANGLE_HEIGHT, ErrorRectangle_data);
        ST7789::draw_string(x + 5, y + 2, text, ST7789::BLACK, error_background_color, DENGB16);
    } else if (state == PROTECT_STATE_WARNING) {
        ST7789::draw_image(x, y, WARNINGRECTANGLE_WIDTH, WARNINGRECTANGLE_HEIGHT, WarningRectangle_data);
        ST7789::draw_string(x + 5, y + 2, text, ST7789::BLACK, warning_background_color, DENGB16);
    }
}

} // namespace SCREEN

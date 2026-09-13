/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 实时测量主页实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-09-13 00:37:29
 */
#include "pages/dashboard_page.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>

#include "blackbox.h"
#include "diagnostic_log.h"
#include "DENGB16.h"
#include "DENGB20.h"
#include "DENGB44_NUM.h"
#include "current_calibration.h"
#include "energy_meter.h"
#include "espnow_link.h"
#include "espnow_service.h"
#include "esp_log.h"
#include "blackbox_service.h"
#include "can_callback.h"
#include "can_resistor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "HXC_NVS.h"
#include "st7789.h"
#include "widgets/ui_chrome.h"
#include "ota_service.h"
#include "wifi_manager.h"
#include "wifi_service.h"

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
const char *DashboardPage::title() const { return "Main"; }

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
        ST7789::fill_screen(ST7789::BLACK);
    const auto state = get_global_state();
    const float voltage = state.voltage_mV / 1000.0f;
    const float current = std::abs(state.current_uA / 1000000.0f);
    char line[32];

    ST7789::fill_rect(166, 6, 1, 99, UI::GRID);
    snprintf(line, sizeof(line), "%.3fV", voltage);
    UI::text(6, 4, 159, 47, line, UI::VOLTAGE, ST7789::BLACK, DENGB44_NUM);
    UI::format_fixed_digits(line, sizeof(line), current, "A", 5, 3, false);
    UI::text(5, 55, 161, 47, line, UI::CURRENT, ST7789::BLACK, DENGB44_NUM);

    const uint32_t seconds = (xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000;
    format_duration(line, sizeof(line), nullptr, seconds);
    UI::text(172, 6, 62, 18, line, ST7789::WHITE, ST7789::BLACK, DENGB16, UI::Align::Center);
    const auto &protection = state.protect_states.states_bit;
    draw_protect_tag(174, 29, "OTP", protection.temperature_protect_state);
    ProtectState_t voltage_state = protection.high_voltage_protect_state;
        const char*    voltage_text  = "OVP";
        if (voltage_state == PROTECT_STATE_NORMAL) {
        voltage_state = protection.low_voltage_protect_state;
            voltage_text  = "UVP";
    }
    draw_protect_tag(174, 55, voltage_text, voltage_state);
    draw_protect_tag(174, 81, "OCP", protection.current_protect_state);

    ST7789::fill_rect(6, 107, 228, 1, UI::GRID);
    UI::badge(6, 113, 24, 16, "W", ST7789::BLACK, UI::POWER, DENGB16);
    UI::format_fixed_digits(line, sizeof(line), voltage * current, "", 5, 3, false);
    UI::text(34, 114, 66, 16, line, UI::POWER, ST7789::BLACK, DENGB16);
    UI::badge(108, 113, 20, 16, "T", UI::MUTED, UI::PANEL, DENGB16);
    const float temperature = state.board_temperature / 100.0f;
        if (temperature >= 100.0f || temperature < 0.0f) {
        snprintf(line, sizeof(line), "%dC", static_cast<int>(temperature));
        } else {
        snprintf(line, sizeof(line), "%.1fC", temperature);
    }
    UI::text(132, 114, 52, 16, line, UI::CURRENT, ST7789::BLACK, DENGB16);
    const bool enabled = state.flags.output_enabled;
    UI::badge(190, 113, 44, 16, enabled ? "ON" : "OFF", ST7789::BLACK, enabled ? UI::CURRENT : UI::VOLTAGE, DENGB16);
}

/**
 * @brief 绘制主页右侧的单个保护状态标签。
 * @param x 标签左上角 X 坐标。
 * @param y 标签左上角 Y 坐标。
 * @param text 标签文本。
 * @param state 保护状态。
 */
void DashboardPage::draw_protect_tag(uint16_t x, uint16_t y, const char* text, ProtectState_t state) {
    if (state == PROTECT_STATE_PROTECT) {
        UI::badge(x, y, 60, 21, text, ST7789::WHITE, UI::VOLTAGE);
    } else if (state == PROTECT_STATE_WARNING) {
        UI::badge(x, y, 60, 21, text, ST7789::BLACK, UI::YELLOW);
    }
    // Normal states stay hidden, as in Lite. Figma's colored tags are examples.
}

} // namespace SCREEN

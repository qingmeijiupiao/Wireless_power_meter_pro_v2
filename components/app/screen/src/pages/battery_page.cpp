/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 累计电量页面实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-09-13 10:10:52
 */
#include "pages/battery_page.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>

#include "blackbox.h"
#include "diagnostic_log.h"
#include "DENGB16.h"
#include "DENGB20.h"
#include "DENGB44_NUM.h"
#include "DENGB28_UNITS.h"
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

// Preserve the 44px font by fitting precision before drawing the combined value/unit.
const char* format_capacity(char* line, size_t size, double milli_value, bool energy) {
    milli_value = std::abs(milli_value);
    for (int scale = milli_value >= 999.5 ? 1 : 0; scale <= 1; ++scale) {
        const double value = scale ? milli_value / 1000.0 : milli_value;
        const char* unit = energy ? (scale ? "Wh" : "mWh") : (scale ? "Ah" : "mAh");
        for (int precision = 3; precision >= 0; --precision) {
            snprintf(line, size, "%.*f", precision, value);
            if (UI::text_width(line, DENGB44_NUM) + UI::text_width(unit, DENGB28_UNITS) + 3 <= 178)
                return unit;
        }
    }
    // Very large accumulated totals retain the base unit with an explicit exponent.
    snprintf(line, size, "%.0e", milli_value / 1000.0);
    return energy ? "Wh" : "Ah";
}

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

/** @brief 返回电量页 ID。 */
PageId BatteryPage::id() const {
    return PageId::Battery;
}

/** @brief 返回电量页标题。 */
const char *BatteryPage::title() const { return "Battery"; }

/** @brief 返回电量页刷新周期。 */
uint32_t BatteryPage::refresh_interval_ms() const {
    return 250;
}

/**
 * @brief 处理电量页按键，侧键长按时重置共享计量会话。
 * @param button 按键 ID。
 * @param event 按键事件。
 * @return true 表示事件已处理。
 */
bool BatteryPage::handle_button(ButtonId button, ButtonEvent event) {
    if (button != ButtonId::Side || event != ButtonEvent::LONG_PRESS) {
        return false;
    }

    EnergyMeter::reset();
    DEVICE_EVENT_I(TAG, "meter: reset source=screen");
    return true;
}

/**
 * @brief 绘制电量页实时状态、累计计量值和时间信息。
 * @param mode 页面渲染模式，电量页始终执行整屏重绘。
 */
void BatteryPage::render(RenderMode mode) {
    (void)mode;
    ST7789::fill_screen(ST7789::BLACK);
    const auto meter = EnergyMeter::snapshot();
    const auto state = get_global_state();
    const float voltage = state.voltage_mV / 1000.0f;
    const float current = state.current_uA / 1000000.0f;
    char line[32];
    const float values[] = {voltage, current, voltage * current};
    const char *units[] = {"V", "A", "W"};
    const ST7789::color_t colors[] = {UI::VOLTAGE, UI::CURRENT, UI::POWER};
    for (uint8_t i = 0; i < 3; ++i) {
        UI::format_fixed_digits(line, sizeof(line), values[i], units[i], 3, 2, true);
        UI::text(6 + i * 49, 5, 48, 18, line, colors[i], ST7789::BLACK, DENGB16);
    }
    format_duration(line, sizeof(line), nullptr, meter.meter_time_ms / 1000);
    UI::text(154, 5, 62, 18, line, UI::CYAN, ST7789::BLACK, DENGB16, UI::Align::Right);
    UI::output_dot(state.flags.output_enabled);
    ST7789::fill_rect(6, 26, 228, 1, UI::GRID);
    const double totals[] = {meter.energy_uwh / 1000.0, meter.charge_uah / 1000.0};
    for (uint8_t i = 0; i < 2; ++i) {
        const uint16_t y = 32 + i * 50;
        const auto color = i == 0 ? UI::POWER : UI::CURRENT;
        UI::badge(9, y + 5, 40, 37, i == 0 ? "W" : "A", ST7789::BLACK, color, DENGB28_UNITS);
        const char* unit = format_capacity(line, sizeof(line), totals[i], i == 0);
        const uint16_t unit_width = UI::text_width(unit, DENGB28_UNITS);
        UI::text(56, y + 3, 178 - unit_width - 3, 47, line, color, ST7789::BLACK, DENGB44_NUM);
        UI::text(234 - unit_width, y + 20, unit_width, 27, unit, color, ST7789::BLACK, DENGB28_UNITS);
    }
}

/** @brief 返回曲线页 ID。 */

} // namespace SCREEN

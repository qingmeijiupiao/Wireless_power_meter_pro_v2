/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 累计电量页面实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-24
 */
#include "pages/battery_page.h"

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
 * @brief 计算 10 的非负整数次幂。
 * @param exponent 指数。
 * @return 10 的 exponent 次幂。
 */
double pow10(uint8_t exponent) {
    double value = 1.0;
    while (exponent-- > 0) {
        value *= 10.0;
    }
    return value;
}

/**
 * @brief 按最大数字位数格式化绝对值，并在末尾附加单位。
 *
 * 小数点和单位不计入 max_digits。数值增大时会逐步减少小数位；
 * clamp 为 true 时，超出显示范围的值会封顶为全 9。
 *
 * @param line 输出缓冲区。
 * @param line_size 输出缓冲区大小。
 * @param value 待格式化数值。
 * @param unit 单位后缀。
 * @param max_digits 最大数字位数。
 * @param max_precision 最多保留的小数位数。
 * @param clamp 是否在超出显示范围时封顶。
 */
void format_fixed_digits(char* line, size_t line_size, double value, const char* unit, uint8_t max_digits,
                         uint8_t max_precision, bool clamp) {
    value         = std::abs(value);
    int precision = max_precision;
    while (precision > 0) {
        const double rounding_limit = pow10(max_digits - precision) - 0.5 / pow10(precision);
        if (value < rounding_limit) {
            break;
        }
        precision--;
    }

    if (clamp && value >= pow10(max_digits) - 0.5) {
        snprintf(line, line_size, "%.*s%s", max_digits, "9999999999", unit);
        return;
    }
    snprintf(line, line_size, "%.*f%s", precision, value, unit);
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
const char* BatteryPage::title() const {
    return "Battery";
}

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

    const EnergyMeter::Snapshot meter          = EnergyMeter::snapshot();
    const int64_t               meter_uwh      = meter.energy_uwh;
    const int64_t               meter_uah      = meter.charge_uah;
    const auto                  global_state   = get_global_state();
    const float                 voltage        = global_state.voltage_mV / 1000.0f;
    const float                 current        = global_state.current_uA / 1000000.0f;
    const float                 power          = voltage * current;
    const uint32_t              system_seconds = (xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000;
    const uint64_t              meter_seconds  = meter.meter_time_ms / 1000;

    char line[32];

    auto draw_realtime_status = [&]() {
        ST7789::draw_image(2, 4, METER_V_LOGO_WIDTH, METER_V_LOGO_HEIGHT, meter_v_logo_data);
        format_fixed_digits(line, sizeof(line), voltage, "V", 3, 2, true);
        ST7789::draw_string(12, 5, line, ST7789::WHITE, ST7789::BLACK, DENGB12);

        ST7789::draw_image(48, 4, METER_A_LOGO_WIDTH, METER_A_LOGO_HEIGHT, meter_a_logo_data);
        format_fixed_digits(line, sizeof(line), current, "A", 3, 2, true);
        ST7789::draw_string(58, 5, line, ST7789::WHITE, ST7789::BLACK, DENGB12);

        ST7789::draw_image(93, 4, METER_W_LOGO_WIDTH, METER_W_LOGO_HEIGHT, meter_w_logo_data);
        format_fixed_digits(line, sizeof(line), power, "W", 3, 2, true);
        ST7789::draw_string(106, 5, line, ST7789::WHITE, ST7789::BLACK, DENGB12);

        const bool output_enabled = global_state.flags.output_enabled;
        ST7789::draw_image(145, 4, output_enabled ? METER_CIRCLE_GREEN_WIDTH : METER_CIRCLE_RED_WIDTH,
                           output_enabled ? METER_CIRCLE_GREEN_HEIGHT : METER_CIRCLE_RED_HEIGHT,
                           output_enabled ? meter_circle_green_data : meter_circle_red_data);
    };

    auto draw_meter_values = [&]() {
        ST7789::draw_image(2, 18, WH_LOGO_WIDTH, WH_LOGO_HEIGHT, wh_logo_data);
        format_fixed_digits(line, sizeof(line), meter_uwh / 1000.0, "mWh", 6, 3, false);
        ST7789::draw_string(34, 20, line, ST7789::color_t(0x003ED0), ST7789::BLACK, DENGB20);

        ST7789::draw_image(2, 43, AH_LOGO_WIDTH, AH_LOGO_HEIGHT, ah_logo_data);
        format_fixed_digits(line, sizeof(line), meter_uah / 1000.0, "mAh", 6, 3, false);
        ST7789::draw_string(34, 45, line, ST7789::color_t(0x1ef851), ST7789::BLACK, DENGB20);
    };

    auto draw_time_values = [&]() {
        format_duration(line, sizeof(line), "S:", system_seconds);
        ST7789::draw_string(2, 68, line, ST7789::color_t(0x2FC9EC), ST7789::BLACK, DENGB12);
        format_duration(line, sizeof(line), "M:", meter_seconds);
        ST7789::draw_string(90, 68, line, ST7789::color_t(0x1EF851), ST7789::BLACK, DENGB12);
    };

    draw_realtime_status();
    draw_meter_values();
    draw_time_values();
}

/** @brief 返回曲线页 ID。 */

} // namespace SCREEN

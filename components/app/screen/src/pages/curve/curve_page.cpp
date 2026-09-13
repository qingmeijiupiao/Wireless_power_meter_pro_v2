/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 历史曲线页面交互、配置与绘制实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-25 16:31:53
 */
#include "pages/curve/curve_page.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "blackbox.h"
#include "diagnostic_log.h"
#include "DENGB16.h"
#include "DENGB20.h"
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

constexpr uint32_t CURVE_WINDOWS_MS[] = {
    10 * 1000,
    30 * 1000,
    2 * 60 * 1000,
    10 * 60 * 1000,
};

constexpr const char* CURVE_WINDOW_TEXT[] = {
    "10s",
    "30s",
    "2m",
    "10m",
};

constexpr float CURVE_MINIMUM_SPANS[] = {
    0.2f,
    0.05f,
    0.5f,
};

struct CurveConfig {
    uint8_t version      = 0; // 配置结构版本
    uint8_t display_mode = 0; // 曲线显示模式
    uint8_t window_index = 0; // 时间窗口索引
    uint8_t reserved     = 0; // 保留字段
};
static_assert(sizeof(CurveConfig) == 4, "CurveConfig size mismatch");

constexpr uint8_t     CURVE_CONFIG_VERSION = 1;
constexpr CurveConfig DEFAULT_CURVE_CONFIG = {
    .version = CURVE_CONFIG_VERSION,
    .display_mode = 0, // CurvePage::DisplayMode::Voltage
    .window_index = 1, // 30s
    .reserved = 0,
};

HXC::NVS_DATA<CurveConfig> curve_config_data("ui_curve_cfg", DEFAULT_CURVE_CONFIG);

/**
 * @brief 将曲线状态数值限制为最多 3 位数字。
 * @param line 输出缓冲区。
 * @param line_size 输出缓冲区大小。
 * @param value 待格式化数值。
 */
void format_curve_value(char* line, size_t line_size, float value) {
    UI::format_fixed_digits(line, line_size, value, "", 3, 2, true);
}

/**
 * @brief 按 1、2、5 倍数生成便于阅读的量程步长。
 * @param value 原始步长。
 * @return 规整后的步长。
 */
float nice_curve_step(float value) {
    if (value <= 0.0f) {
        return 1.0f;
    }

    const float exponent = std::floor(std::log10(value));
    const float scale    = std::pow(10.0f, exponent);
    const float fraction = value / scale;
    if (fraction <= 1.0f) {
        return scale;
    }
    if (fraction <= 2.0f) {
        return 2.0f * scale;
    }
    if (fraction <= 5.0f) {
        return 5.0f * scale;
    }
    return 10.0f * scale;
}

// Shared geometry/text widget; edit outlines retain the original behavior.
void draw_curve_badge(uint16_t x, uint16_t y, uint16_t width, const char* text, ST7789::color_t foreground,
                      ST7789::color_t background, bool outlined = false, ST7789::color_t outline_color = UI::YELLOW) {
    UI::badge(x, y, width, 20, text, foreground, background, DENGB16);
    if (outlined) {
        ST7789::draw_round_rect(x, y, width, 20, 4, 1, outline_color, background);
        UI::text(x + 3, y, width - 6, 20, text, foreground, background, DENGB16, UI::Align::Center);
    }
}

} // namespace

PageId CurvePage::id() const {
    return PageId::Curve;
}

/** @brief 返回曲线页标题。 */
const char *CurvePage::title() const { return "Curve"; }

/** @brief 返回曲线页刷新周期。 */
uint32_t CurvePage::refresh_interval_ms() const {
    return 200;
}

void CurvePage::on_enter() {
    if (!config_loaded_) {
        load_config();
    }
}

/** @brief 声明曲线页支持参数编辑模式。 */
bool CurvePage::supports_edit_mode() const {
    return true;
}

/** @brief 返回曲线页是否处于参数编辑状态。 */
bool CurvePage::is_overlay_active() const {
    return editing_;
}

/** @brief 进入曲线参数编辑状态。 */
void CurvePage::on_edit_enter() {
    editing_   = true;
    edit_item_ = EditItem::Display;
}

/** @brief 退出曲线参数编辑状态。 */
void CurvePage::on_edit_exit() {
    editing_ = false;
}

/**
 * @brief 处理曲线页显示模式和时间跨度切换。
 * @param button 按键 ID。
 * @param event 按键事件。
 * @return true 表示事件已由曲线页消费。
 */
bool CurvePage::handle_button(ButtonId button, ButtonEvent event) {
    if (!editing_) {
        if (button == ButtonId::Side && event == ButtonEvent::DOUBLE_CLICK) {
            display_mode_ = static_cast<DisplayMode>((static_cast<uint8_t>(display_mode_) + 1) %
                                                     static_cast<uint8_t>(DisplayMode::Count));
            save_config();
            return true;
        }
        return false;
    }

    if (button == ButtonId::Side && event == ButtonEvent::SHORT_PRESS) {
        edit_item_ =
            static_cast<EditItem>((static_cast<uint8_t>(edit_item_) + 1) % static_cast<uint8_t>(EditItem::Count));
        return true;
    }

    if (button == ButtonId::Main && event == ButtonEvent::SHORT_PRESS) {
        if (edit_item_ == EditItem::Display) {
            display_mode_ = static_cast<DisplayMode>((static_cast<uint8_t>(display_mode_) + 1) %
                                                     static_cast<uint8_t>(DisplayMode::Count));
        } else {
            window_index_ = (window_index_ + 1) % (sizeof(CURVE_WINDOWS_MS) / sizeof(CURVE_WINDOWS_MS[0]));
            // 时间窗口改变后立即重新建立量程，避免沿用旧窗口的极值范围。
            for (auto& range : ranges_) {
                range.initialized         = false;
                range.shrink_candidate_ms = 0;
            }
        }
        save_config();
        return true;
    }

    if (button == ButtonId::Side && event == ButtonEvent::LONG_PRESS) {
        editing_ = false;
        return true;
    }

    return true;
}

uint32_t CurvePage::window_ms() const {
    return CURVE_WINDOWS_MS[window_index_];
}

const char* CurvePage::display_mode_text() const {
    switch (display_mode_) {
    case DisplayMode::Voltage:
        return "V";
    case DisplayMode::Current:
        return "A";
    case DisplayMode::Power:
        return "W";
    case DisplayMode::All:
        return "ALL";
    default:
        return "?";
    }
}

const char *CurvePage::window_text() const { return CURVE_WINDOW_TEXT[window_index_]; }

void CurvePage::load_config() {
    const CurveConfig config = curve_config_data.read();
    const bool        valid  = config.version == CURVE_CONFIG_VERSION &&
                       config.display_mode < static_cast<uint8_t>(DisplayMode::Count) &&
                       config.window_index < sizeof(CURVE_WINDOWS_MS) / sizeof(CURVE_WINDOWS_MS[0]);

    if (valid) {
        display_mode_ = static_cast<DisplayMode>(config.display_mode);
        window_index_ = config.window_index;
    } else {
        display_mode_       = DisplayMode::Voltage;
        window_index_       = 1;
        const esp_err_t ret = curve_config_data.set(DEFAULT_CURVE_CONFIG);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to restore curve config: %s", esp_err_to_name(ret));
        }
    }
    config_loaded_ = true;
}

void CurvePage::save_config() const {
    const CurveConfig config = {
        .version      = CURVE_CONFIG_VERSION,
        .display_mode = static_cast<uint8_t>(display_mode_),
        .window_index = window_index_,
        .reserved     = 0,
    };
    const esp_err_t ret = curve_config_data.set(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to save curve config: %s", esp_err_to_name(ret));
    }
}

void CurvePage::update_auto_range(CurveMetric metric, const CurveBucket* buckets, size_t bucket_count,
                                  uint32_t now_ms) {
    bool  have_data    = false;
    float data_minimum = 0.0f;
    float data_maximum = 0.0f;
    for (size_t i = 0; i < bucket_count; ++i) {
        if (!buckets[i].valid) {
            continue;
        }
        if (!have_data) {
            data_minimum = buckets[i].minimum;
            data_maximum = buckets[i].maximum;
            have_data    = true;
        } else {
            data_minimum = std::min(data_minimum, buckets[i].minimum);
            data_maximum = std::max(data_maximum, buckets[i].maximum);
        }
    }
    if (!have_data) {
        return;
    }

    const uint8_t metric_index   = static_cast<uint8_t>(metric);
    const float   minimum_span   = CURVE_MINIMUM_SPANS[metric_index];
    float         span           = std::max(data_maximum - data_minimum, minimum_span);
    float         target_minimum = data_minimum - span * 0.1f;
    float         target_maximum = data_maximum + span * 0.1f;

    // 电流和功率均使用绝对值，接近零时固定零点能提高量程可读性。
    if (metric != CurveMetric::Voltage && target_minimum < span * 0.08f) {
        target_minimum = 0.0f;
    }

    const float nice_step = nice_curve_step((target_maximum - target_minimum) / 4.0f);
    target_minimum        = std::floor(target_minimum / nice_step) * nice_step;
    target_maximum        = std::ceil(target_maximum / nice_step) * nice_step;
    if (target_maximum - target_minimum < minimum_span) {
        target_maximum = target_minimum + minimum_span;
    }

    AutoRange& range = ranges_[metric_index];
    if (!range.initialized) {
        range.minimum     = target_minimum;
        range.maximum     = target_maximum;
        range.initialized = true;
        return;
    }

    // 峰值超出当前范围时立即扩张，确保突发浪涌不会被裁剪。
    if (target_minimum < range.minimum || target_maximum > range.maximum) {
        range.minimum             = std::min(range.minimum, target_minimum);
        range.maximum             = std::max(range.maximum, target_maximum);
        range.shrink_candidate_ms = 0;
        return;
    }

    const bool can_shrink = target_minimum > range.minimum || target_maximum < range.maximum;
    if (!can_shrink) {
        range.shrink_candidate_ms = 0;
        return;
    }

    if (range.shrink_candidate_ms == 0) {
        range.shrink_candidate_ms = now_ms;
        return;
    }
    if (now_ms - range.shrink_candidate_ms < 2000) {
        return;
    }

    // 收缩采用渐进逼近，避免旧峰值离开窗口时画面突然跳变。
    range.minimum += (target_minimum - range.minimum) * 0.25f;
    range.maximum += (target_maximum - range.maximum) * 0.25f;
}

void CurvePage::draw_grid(uint16_t x, uint16_t y, uint16_t width, uint16_t height) const {
    const ST7789::color_t grid_color(0x242424);
    for (uint8_t i = 0; i <= 4; ++i) {
        const uint16_t grid_x = x + (width - 1) * i / 4;
        ST7789::draw_line(grid_x, y, grid_x, y + height - 1, grid_color);
        const uint16_t grid_y = y + (height - 1) * i / 4;
        ST7789::draw_line(x, grid_y, x + width - 1, grid_y, grid_color);
    }
}

void CurvePage::draw_bucket_curve(const CurveBucket* buckets, size_t bucket_count, const AutoRange& range, uint16_t x,
                                  uint16_t y, uint16_t height, ST7789::color_t color) const {
    if (!range.initialized || range.maximum <= range.minimum) {
        return;
    }

    auto map_y = [&](float value) {
        const float normalized = std::clamp((value - range.minimum) / (range.maximum - range.minimum), 0.0f, 1.0f);
        return static_cast<int16_t>(y + height - 1 - std::lround(normalized * static_cast<float>(height - 1)));
    };

    bool    have_previous = false;
    int16_t previous_x    = 0;
    int16_t previous_y    = 0;
    for (size_t i = 0; i < bucket_count; ++i) {
        if (!buckets[i].valid) {
            have_previous = false;
            continue;
        }

        const int16_t point_x   = static_cast<int16_t>(x + i);
        const int16_t minimum_y = map_y(buckets[i].minimum);
        const int16_t maximum_y = map_y(buckets[i].maximum);
        const int16_t average_y = map_y(buckets[i].average);

        // 像素桶竖线保留瞬时峰谷，平均值连线表达整体趋势。
        ST7789::draw_line(point_x, maximum_y, point_x, minimum_y, color);
        if (have_previous) {
            ST7789::draw_line(previous_x, previous_y, point_x, average_y, color);
        }
        previous_x    = point_x;
        previous_y    = average_y;
        have_previous = true;
    }
}

void CurvePage::draw_single_metric(CurveMetric metric, ST7789::color_t color) {
    constexpr uint16_t plot_x = 50;
    constexpr uint16_t plot_y = 32;
    constexpr uint16_t plot_width = ST7789::WIDTH - plot_x - 5;
    constexpr uint16_t plot_height = ST7789::HEIGHT - plot_y - 6;
    CurveHistory::instance().build_buckets(metric, window_ms(), buckets_, plot_width);
    const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    update_auto_range(metric, buckets_, plot_width, now_ms);

    draw_grid(plot_x, plot_y, plot_width, plot_height);
    draw_bucket_curve(buckets_, plot_width, ranges_[static_cast<uint8_t>(metric)], plot_x, plot_y, plot_height, color);

    bool have_data = false;
    float visible_minimum = 0.0f;
    float visible_maximum = 0.0f;
    for (size_t i = 0; i < plot_width; ++i) {
        const CurveBucket& bucket = buckets_[i];
        if (!bucket.valid) {
            continue;
        }
        if (!have_data) {
            visible_minimum = bucket.minimum;
            visible_maximum = bucket.maximum;
            have_data = true;
        } else {
            visible_minimum = std::min(visible_minimum, bucket.minimum);
            visible_maximum = std::max(visible_maximum, bucket.maximum);
        }
    }

    if (have_data) {
        char maximum_text[10];
        char minimum_text[10];
        format_curve_value(maximum_text, sizeof(maximum_text), visible_maximum);
        format_curve_value(minimum_text, sizeof(minimum_text), visible_minimum);

        UI::badge(6, 33, 42, 20, "MAX", ST7789::BLACK, UI::YELLOW, DENGB16);
        UI::text(6, 58, 38, 22, maximum_text, UI::YELLOW, ST7789::BLACK, DENGB16, UI::Align::Center);
        UI::text(6, 86, 38, 22, minimum_text, UI::CYAN, ST7789::BLACK, DENGB16, UI::Align::Center);
        UI::badge(6, 110, 38, 19, "MIN", ST7789::BLACK, UI::CYAN, DENGB16);
    }
}

void CurvePage::draw_all_metrics() {
    constexpr uint16_t plot_x = 6;
    constexpr uint16_t plot_y = 32;
    constexpr uint16_t plot_width = ST7789::WIDTH - 12;
    constexpr uint16_t plot_height = ST7789::HEIGHT - plot_y - 6;
    constexpr CurveMetric metrics[]   = {
        CurveMetric::Voltage,
        CurveMetric::Current,
        CurveMetric::Power,
    };
    const ST7789::color_t colors[] = {
        ST7789::color_t(0xef2a2a),
        ST7789::color_t(0x1ef851),
        ST7789::color_t(0x469CFF),
    };
    const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    draw_grid(plot_x, plot_y, plot_width, plot_height);
    for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
        CurveHistory::instance().build_buckets(metrics[i], window_ms(), buckets_, plot_width);
        update_auto_range(metrics[i], buckets_, plot_width, now_ms);
        draw_bucket_curve(buckets_, plot_width, ranges_[static_cast<uint8_t>(metrics[i])], plot_x, plot_y, plot_height,
                          colors[i]);
    }
}

/**
 * @brief 绘制实时曲线、当前模式和时间跨度。
 * @param mode 页面渲染模式。
 */
void CurvePage::render(RenderMode mode) {
    (void)mode;
    ST7789::fill_screen(ST7789::BLACK);

    const ST7789::color_t voltage_color(0xef2a2a);
    const ST7789::color_t current_color(0x1ef851);
    const ST7789::color_t power_color(0x469CFF);
    const ST7789::color_t time_color(0x2FC9EC);
    const ST7789::color_t now_color(0xF2C94C);
    const ST7789::color_t value_background(0x101010);
    const bool            display_selected = editing_ && edit_item_ == EditItem::Display;
    const bool            window_selected  = editing_ && edit_item_ == EditItem::TimeWindow;

    ST7789::color_t mode_color      = voltage_color;
    ST7789::color_t mode_foreground = ST7789::WHITE;
    if (display_mode_ == DisplayMode::Current) {
        mode_color      = current_color;
        mode_foreground = ST7789::BLACK;
    } else if (display_mode_ == DisplayMode::Power) {
        mode_color = power_color;
    } else if (display_mode_ == DisplayMode::All) {
        mode_color      = ST7789::YELLOW;
        mode_foreground = ST7789::BLACK;
    }
    draw_curve_badge(6, 5, 36, display_mode_text(), mode_foreground, mode_color, display_selected);

    if (display_mode_ == DisplayMode::All) {
        draw_curve_badge(44, 5, 30, "V", ST7789::WHITE, voltage_color);
        draw_curve_badge(79, 5, 30, "A", ST7789::BLACK, current_color);
        draw_curve_badge(114, 5, 30, "W", ST7789::WHITE, power_color);
    } else {
        const auto  state    = get_global_state();
        const float voltage  = state.voltage_mV / 1000.0f;
        const float current = std::abs(state.current_uA / 1000000.0f);
        const float value    = display_mode_ == DisplayMode::Voltage
                                                          ? voltage
                                                          : (display_mode_ == DisplayMode::Current ? current : voltage * current);
        char current_text[16];
        format_curve_value(current_text, sizeof(current_text), value);
        draw_curve_badge(46, 5, 46, "NOW", ST7789::BLACK, now_color);
        draw_curve_badge(96, 5, 54, current_text, mode_color, value_background, true, mode_color);
    }
    draw_curve_badge(154, 5, 54, window_text(), ST7789::BLACK, time_color, window_selected);

    const bool output_enabled = get_global_state().flags.output_enabled;
    UI::output_dot(output_enabled);

    switch (display_mode_) {
    case DisplayMode::Voltage:
        draw_single_metric(CurveMetric::Voltage, voltage_color);
        break;
    case DisplayMode::Current:
        draw_single_metric(CurveMetric::Current, current_color);
        break;
    case DisplayMode::Power:
        draw_single_metric(CurveMetric::Power, power_color);
        break;
    case DisplayMode::All:
        draw_all_metrics();
        break;
    default:
        break;
    }
}

/** @brief 返回无线页 ID。 */

} // namespace SCREEN

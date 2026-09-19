/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 页面级公共装饰绘制实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-24
 */
#include "widgets/ui_chrome.h"

#include "st7789.h"
#include "DENGB20.h"
#include "DENGB28_NUM.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>

namespace SCREEN {

void draw_edit_indicator() {
    ST7789::fill_rect(0, 0, ST7789::WIDTH, 1, ST7789::YELLOW);
}

namespace UI {
void short_circuit_dialog(bool is_short, uint16_t voltage_mV, uint16_t threshold_mV) {
    ST7789::fill_round_rect(8, 8, 224, 119, 6, ST7789::BLACK, ST7789::BLACK);
    ST7789::draw_round_rect(8, 8, 224, 119, 6, 1, VOLTAGE, ST7789::BLACK);
    text(16, 14, 208, 20, is_short ? "SHORT CIRCUIT" : "CHECK FAILED", VOLTAGE,
         ST7789::BLACK, DENGB20);
    ST7789::fill_rect(16, 39, 208, 1, GRID);
    text(16, 47, 208, 18, "OUTPUT BLOCKED", ST7789::WHITE);
    char detail[40];
    if (is_short) {
        snprintf(detail, sizeof(detail), "%u mV / min %u", voltage_mV, threshold_mV);
    } else {
        snprintf(detail, sizeof(detail), "Check detector / wiring");
    }
    text(16, 70, 208, 18, detail, MUTED);
    text(16, 101, 208, 18, "Any key: dismiss", YELLOW);
}

/**
 * @brief 计算 10 的非负整数次幂。
 * @param exponent 指数。
 * @return 10 的 exponent 次幂。
 */
static double pow10(uint8_t exponent) {
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


namespace {
uint8_t glyph_index(unsigned char c) { return (c >= 32 && c <= 126 ? c : '?') - 32; }
bool supports(const char *value, const Font_t &font) {
    for (; *value; ++value) {
        if (font.width_table[glyph_index(static_cast<unsigned char>(*value))] == 0)
            return false;
}
    return true;
}
} // namespace

uint16_t text_width(const char *value, const Font_t &font) {
    uint32_t width = 0;
    if (value)
        for (; *value; ++value)
            width += font.width_table[glyph_index(static_cast<unsigned char>(*value))];
    return static_cast<uint16_t>(std::min<uint32_t>(width, UINT16_MAX));
}

void text(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *value, ST7789::color_t fg, ST7789::color_t bg,
          const Font_t &preferred, Align align) {
    if (!value || !w || !h)
        return;
    const Font_t *font = &DENGB16;
    const Font_t *candidates[] = {&preferred, &DENGB20, &DENGB16};
    for (const auto *candidate : candidates) {
        if (candidate->font_height <= preferred.font_height && candidate->font_height <= h &&
            supports(value, *candidate) && text_width(value, *candidate) <= w) {
            font = candidate;
            break;
}
}
    if (font->font_height > h)
        return;
    char clipped[96];
    if (text_width(value, *font) > w) {
        const uint16_t suffix_width = text_width("...", *font);
        if (suffix_width > w)
            return;
        size_t length = 0;
        uint16_t width = 0;
        while (value[length] && length < sizeof(clipped) - 4) {
            const auto advance = font->width_table[glyph_index(static_cast<unsigned char>(value[length]))];
            if (width + advance + suffix_width > w)
                break;
            clipped[length] = value[length];
            width += advance;
            ++length;
}
        std::memcpy(clipped + length, "...", 4);
        value = clipped;
}
    const uint16_t width = text_width(value, *font);
    const uint16_t offset = align == Align::Right ? w - width : (align == Align::Center ? (w - width) / 2 : 0);
    ST7789::draw_string(x + offset, y + (h - font->font_height) / 2, value, fg, bg, *font);
}

void number(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *value, ST7789::color_t color) {
    text(x, y, w, h, value, color, ST7789::BLACK, DENGB28_NUM);
}

void badge(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *value, ST7789::color_t fg, ST7789::color_t bg,
           const Font_t &font) {
    ST7789::fill_round_rect(x, y, w, h, 4, bg, ST7789::BLACK);
    if (w > 6)
        text(x + 3, y, w - 6, h, value, fg, bg, font, Align::Center);
}

namespace {
OutputView current_output_view;
ST7789::color_t output_color() {
    switch (current_output_view.state) {
    case OutputVisual::On: return CURRENT;
    case OutputVisual::Checking:
    case OutputVisual::Wait: return YELLOW;
    case OutputVisual::Locked:
    case OutputVisual::Error: return VOLTAGE;
    default: return MUTED;
    }
}
} // namespace

void set_output_view(const OutputView& view) { current_output_view = view; }
const OutputView& output_view() { return current_output_view; }

void output_capsule(uint16_t x, uint16_t y) {
    const auto& view = current_output_view;
    const auto color = output_color();
    // 纯黑底只保留边框和状态色，文字背景与主页其他区域一致。
    ST7789::fill_round_rect(x, y, 72, 23, 6, ST7789::BLACK, ST7789::BLACK);
    ST7789::draw_round_rect(x, y, 72, 23, 6, view.pressed ? 2 : 1,
                           view.pressed ? ST7789::WHITE : color, ST7789::BLACK);
    if (view.state == OutputVisual::Checking) {
        for (uint8_t i = 0; i < 3; ++i)
            ST7789::fill_rect(x + 5, y + 6 + i * 4, 4, 3, i == view.animation ? color : GRID);
    } else {
        ST7789::fill_round_rect(x + 5, y + 8, 7, 7, 3, color, ST7789::BLACK);
    }
    text(x + 15, y + 3, 52, 17, view.label, color, ST7789::BLACK, DENGB16, Align::Center);
    // 旁路使用独立蓝色标记，避免与 ON 的绿色混为一谈。
    if (view.bypassed) ST7789::fill_rect(x + 23, y + 20, 26, 2, CYAN);
}

void output_dot(bool enabled) {
    (void)enabled; // 统一使用屏幕任务更新的输出展示快照。
    const auto& view = current_output_view;
    ST7789::fill_round_rect(220, 8, 14, 14, 7, output_color(), ST7789::BLACK);
    if (view.state == OutputVisual::Checking)
        ST7789::fill_rect(223 + view.animation * 3, 13, 3, 4, ST7789::BLACK);
    if (view.pressed) ST7789::draw_round_rect(218, 6, 18, 18, 8, 1, ST7789::WHITE, ST7789::BLACK);
    if (view.bypassed) ST7789::fill_rect(222, 24, 10, 2, CYAN);
}

void output_feedback_overlay(bool dashboard) {
    const auto& view = current_output_view;
    const bool transient = view.detail[0] || (!dashboard &&
        (view.pressed || view.state == OutputVisual::Checking || view.state == OutputVisual::Wait));
    if (!transient) return;
    ST7789::fill_rect(4, 109, 154, 25, ST7789::BLACK);
    const char* detail = view.detail[0] ? view.detail : view.state == OutputVisual::Checking ? "CHECKING OUTPUT" :
                         view.state == OutputVisual::Wait ? "PLEASE WAIT" : "BUTTON PRESSED";
    text(6, 112, 150, 20, detail, output_color());
    if (!dashboard) output_capsule();
}
} // namespace UI

} // namespace SCREEN

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

void output_dot(bool enabled) {
    ST7789::fill_round_rect(220, 8, 14, 14, 7, enabled ? CURRENT : VOLTAGE, ST7789::BLACK);
}
} // namespace UI

} // namespace SCREEN

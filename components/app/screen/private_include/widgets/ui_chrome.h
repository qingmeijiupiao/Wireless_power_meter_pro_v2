#include <cstddef>
/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 页面级公共装饰绘制接口
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-24
 */
#ifndef SCREEN_UI_CHROME_H
#define SCREEN_UI_CHROME_H

#include "st7789.h"
#include "DENGB16.h"
#include "widgets/output_view.h"

namespace SCREEN {

/**
 * @brief 绘制页面编辑状态的顶部提示线。
 */
void draw_edit_indicator();

namespace UI {
// 最大数字位数不包含小数点和单位后缀。
void format_fixed_digits(char* line, size_t line_size, double value, const char* unit,
                         uint8_t max_digits, uint8_t max_precision, bool clamp);
// PRO V2 公共配色；控件采用程序绘制，不存储位图。
inline const ST7789::color_t PANEL(0x202020);
inline const ST7789::color_t MUTED(0xA5ABB5);
inline const ST7789::color_t VOLTAGE(0xEF2A2A);
inline const ST7789::color_t CURRENT(0x1EF851);
inline const ST7789::color_t POWER(0x469CFF);
inline const ST7789::color_t YELLOW(0xFFCC00);
inline const ST7789::color_t CYAN(0x2FC9EC);
inline const ST7789::color_t GRID(0x242424);

enum class Align { Left, Center, Right };
uint16_t text_width(const char *text, const Font_t &font);
// 通过选择已有小字号适配宽度，不丢弃小数位；超长文本在自己的边界内显示省略号。
void text(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *value, ST7789::color_t fg,
          ST7789::color_t bg = ST7789::BLACK, const Font_t &preferred = DENGB16, Align align = Align::Left);
void number(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *value, ST7789::color_t color);
void badge(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *value, ST7789::color_t fg, ST7789::color_t bg,
           const Font_t &font = DENGB16);
void output_dot(bool enabled);
/** 主页完整状态胶囊；其他页面的输出图标共享相同显示数据。 */
void output_capsule(uint16_t x = 162, uint16_t y = 110);
/** 短提示只覆盖底栏；其他页面检测中临时显示紧凑状态。 */
void output_feedback_overlay(bool dashboard);
/** 全局开启前保护提示；由 screen_task 在页面绘制后调用。 */
void short_circuit_dialog(bool is_short, uint16_t voltage_mV, uint16_t threshold_mV);
} // namespace UI

} // namespace SCREEN

#endif // SCREEN_UI_CHROME_H

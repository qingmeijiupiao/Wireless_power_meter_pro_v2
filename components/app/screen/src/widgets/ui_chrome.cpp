/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 页面级公共装饰绘制实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-24
 */
#include "widgets/ui_chrome.h"

#include "st7789.h"

namespace SCREEN {

void draw_edit_indicator() {
    ST7789::fill_rect(0, 0, ST7789::WIDTH, 1, ST7789::YELLOW);
}

} // namespace SCREEN

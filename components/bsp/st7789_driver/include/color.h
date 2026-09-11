/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 颜色类，用于表示和操作颜色值
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-01 18:34:35
 */
#ifndef __COLOR_H__
#define __COLOR_H__

#include <stdint.h>
namespace ST7789 {
class color_t {
  public:
    /** @brief `color_t` 接口。 */
    color_t() {
        color_raw = 0;
    };
    /** @brief `color_t` 接口。 */
    color_t(uint8_t r, uint8_t g, uint8_t b) {
        color_raw = (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    };
    /** @brief `color_t` 接口。 */
    color_t(uint32_t color_hex) : color_t((color_hex >> 16) & 0xFF, (color_hex >> 8) & 0xFF, color_hex & 0xFF) {};

    /** @brief `get_color_raw` 接口。 */
    uint16_t get_color_raw() { // 获取小端序颜色值
        return color_raw;
    }
    /** @brief `get_color_raw_big_endian` 接口。 */
    uint16_t get_color_raw_big_endian() { // 获取大端序颜色值
        return (color_raw << 8) | (color_raw >> 8);
    }
    /** @brief `set_color_raw` 接口。 */
    void set_color_raw(uint16_t color_raw) {
        this->color_raw = color_raw;
    }
    /** @brief `set_color_raw_big_endian` 接口。 */
    void set_color_raw_big_endian(uint16_t color_raw) {
        this->color_raw = (color_raw << 8) | (color_raw >> 8);
    }

  protected:
    uint16_t color_raw; // RGB565颜色值小端序
};

const color_t BLACK(0, 0, 0);       /**< 黑色 */
const color_t WHITE(255, 255, 255); /**< 白色 */
const color_t RED(255, 0, 0);       /**< 红色 */
const color_t GREEN(0, 255, 0);     /**< 绿色 */
const color_t BLUE(0, 0, 255);      /**< 蓝色 */
const color_t YELLOW(255, 255, 0);  /**< 黄色 */

} // namespace ST7789

#endif

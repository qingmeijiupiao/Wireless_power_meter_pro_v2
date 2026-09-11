/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 屏幕方向、背光配置与持久化接口
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-24 22:55:08
 */
#ifndef UI_COMMON_H
#define UI_COMMON_H

#include <stdint.h>

#include "HXC_NVS.h"
#include "st7789.h"

namespace SCREEN {

/** 背光设置固定为 5 档，便于单按键循环调整 */
static constexpr uint8_t BACKLIGHT_LEVEL_COUNT = 5;

/** 默认背光档位，取中间偏亮档 */
static constexpr uint8_t DEFAULT_BACKLIGHT_LEVEL = 3;

/**
 * @brief 归一化背光档位
 * @param level 背光档位，范围 1-BACKLIGHT_LEVEL_COUNT
 * @return 合法背光档位
 */
uint8_t normalize_backlight_level(uint8_t level);

/**
 * @brief 将 n 档背光映射为 ST7789 背光值
 * @param level 背光档位，范围 1-BACKLIGHT_LEVEL_COUNT
 * @return ST7789 背光值，范围 0-255
 */
uint8_t backlight_value_from_level(uint8_t level);

/**
 * @brief 获取已保存的 180 度旋转配置
 * @return true 表示 180 度旋转
 */
bool ui_config_get_rotation_180();

/**
 * @brief 保存 180 度旋转配置
 * @param enabled true 表示 180 度旋转
 */
esp_err_t ui_config_set_rotation_180(bool enabled);

/**
 * @brief 获取已保存的背光档位
 * @return 背光档位，范围 1-BACKLIGHT_LEVEL_COUNT
 */
uint8_t ui_config_get_backlight_level();

/**
 * @brief 保存背光档位
 * @param level 背光档位，范围 1-BACKLIGHT_LEVEL_COUNT
 */
esp_err_t ui_config_set_backlight_level(uint8_t level);

} // namespace SCREEN

#endif

/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 屏幕 UI 应用组件公开接口，提供屏幕任务入口和跨任务按键事件投递接口
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-05-30
 */
#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

#include "Button.h"
#include "esp_err.h"

namespace SCREEN {

/** 默认开机画面显示时长，单位 ms。 */
static constexpr uint32_t DEFAULT_START_LOGO_DURATION_MS = 2000;
/** 开机画面最大显示时长，单位 ms。 */
static constexpr uint32_t MAX_START_LOGO_DURATION_MS     = 10000;

/**
 * @brief UI 层识别的物理按键
 */
enum class ButtonId : uint8_t {
    Main, /**< 正面主按键 */
    Side, /**< 侧边功能按键 */
    Previous, /**< 上一页/BOOT 复用按键 */
};

/**
 * @brief 屏幕任务入口
 *
 * 初始化 ST7789 屏幕、UI 管理器和显示配置，随后持续处理 UI 事件并刷新当前页面。
 * @param arg FreeRTOS 任务参数，当前未使用
 */
void screen_task(void* arg);

/** @brief 绑定应用按键事件并初始化对应的 GPIO 输入。 */
esp_err_t init_buttons();

/**
 * @brief 获取持久化保存的开机画面显示时长。
 * @return 显示时长，单位 ms；返回 0
 * 表示关闭开机画面。

 */
uint32_t get_start_logo_duration_ms();

/**
 * @brief 设置并持久化开机画面显示时长。
 * @param duration_ms 显示时长，单位 ms；传入 0 表示关闭开机画面。

 * *
 * @return ESP_OK 表示保存成功，其他值表示 NVS 写入失败。
 */
esp_err_t set_start_logo_duration_ms(uint32_t duration_ms);

/**
 * @brief 向屏幕任务投递按键事件
 *
 * 按键回调运行在 Button 组件任务中，不能直接修改 UI 状态；该接口只负责将事件写入
 * UIManager 队列，由 screen_task 在线程内统一消费。
 * @param button 按键 ID
 * @param event 按键事件
 * @return true 表示事件已写入队列
 */
bool post_button_event(ButtonId button, ButtonEvent event);

} // namespace SCREEN

#endif

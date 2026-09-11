/*
 * @Description:  硬件配置和不同硬件版本适配模块头文件
 * @Author: qingmeijiupiao
 * @version: 1.0.0
 * @Date: 2026-04-25 00:49:51
 * @LastEditTime: 2026-04-29 00:34:58
 */
#ifndef HRADWARE_H
#define HRADWARE_H
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include <stdint.h>

constexpr adc_channel_t hardware_adc_channel = ADC_CHANNEL_0;

struct hardware_config {
    // TFT屏幕引脚配置
    gpio_num_t TFT_SCL;
    gpio_num_t TFT_SDA;
    gpio_num_t TFT_RST;
    gpio_num_t TFT_RS;
    gpio_num_t TFT_CS;
    gpio_num_t TFT_BLK;
    bool       TFT_BLK_ACTIVE_STATE; // 备光开启时引脚的电平状态

    // 温度传感器通道
    adc_channel_t temperature_channel;
    adc_channel_t short_detect_channel;

    // CAN 引脚配置
    gpio_num_t CAN_TX;
    gpio_num_t CAN_RX;
    gpio_num_t CAN_RESISTOR_ENABLE; // CAN电阻使能引脚

    // ina 226 引脚配置
    gpio_num_t INAA226_SDA;
    gpio_num_t INAA226_SCL;
    gpio_num_t INAA226_ALERT; // 中断引脚

    // 输出控制引脚配置
    gpio_num_t OUTPUT_CTRL; // 输出控制引脚
    gpio_num_t SHORT_TEST_ENABLE;

    // 按键引脚配置
    gpio_num_t MAIN_BUTTON; // 主按键引脚
    gpio_num_t SIDE_BUTTON; // 侧边上的按键引脚
    gpio_num_t PREVIOUS_BUTTON; // 上一页/BOOT 复用按键
};

/**
 * @description: 初始化硬件配置
 * @note: 该函数必须是第一个被调用的函数，用于初始化硬件版本配置
 * @return {*}
 */
esp_err_t hardware_config_init();

/**
 * @description: 获取硬件版本
 * @return {uint8_t} 硬件版本号，255表示未知版本
 */
uint8_t get_hardware_version();

/**
 * @description: 获取硬件配置
 * @return {const struct hardware_config&} 硬件配置参数引用
 */
const struct hardware_config& get_hardware_config();

/** 读取短路检测 ADC 原始值。 */
esp_err_t read_short_detect_raw(int& raw);

/** 控制短路检测测试激励；上电默认关闭。 */
esp_err_t set_short_test_enabled(bool enabled);

#endif

/**
 * @file short_circuit_detect.h
 * @brief 输出端短路检测组件公共接口。
 */
#ifndef SHORT_CIRCUIT_DETECT_H
#define SHORT_CIRCUIT_DETECT_H

#include <cstdint>

#include "esp_err.h"

namespace ShortCircuitDetect {

/** 默认短路判定阈值，单位 mV。 */
constexpr uint16_t DEFAULT_THRESHOLD_MV = 200;

/** 允许配置的最大阈值，受 ADC 输入量程限制。 */
constexpr uint16_t MAX_THRESHOLD_MV = 3300;

/** 单次短路检测结果。 */
struct Result {
    uint16_t voltage_mV;   /**< 多次采样后的平均检测电压。 */
    uint16_t threshold_mV; /**< 本次判定使用的阈值。 */
    uint8_t  sample_count; /**< 成功参与平均的采样数量。 */
    bool     is_short;     /**< true 表示检测电压低于阈值。 */
};

/**
 * @brief 初始化组件并确保 U_SHOT_TEST 保持低电平。
 * @return ESP_OK 成功；其他值表示测试 GPIO 初始化状态异常。
 */
esp_err_t init();

/**
 * @brief 执行一次短路检测。
 *
 * 函数短时拉高 U_SHOT_TEST，经二极管和 100 欧姆电阻向输出正极施加
 * 测试电压，然后读取 U_SHOT_DETECT。退出前始终恢复测试引脚为低电平。
 *
 * @param result 返回测量电压、阈值和判定结果。
 * @return ESP_OK 检测完成；其他值表示 GPIO 或 ADC 操作失败。
 */
esp_err_t test(Result& result);

/** @return 当前 NVS 中的短路判定阈值，单位 mV。 */
uint16_t get_threshold_mV();

/**
 * @brief 设置短路判定阈值并保存到 NVS。
 * @param threshold_mV 阈值，范围 1～MAX_THRESHOLD_MV mV。
 * @return ESP_OK 保存成功；ESP_ERR_INVALID_ARG 参数超出范围。
 */
esp_err_t set_threshold_mV(uint16_t threshold_mV);

} // namespace ShortCircuitDetect

#endif // SHORT_CIRCUIT_DETECT_H

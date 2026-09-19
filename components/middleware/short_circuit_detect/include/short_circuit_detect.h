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
constexpr uint32_t MAX_TEST_TIME_MS = 500;
constexpr uint32_t SAMPLE_INTERVAL_MS = 10;
constexpr uint8_t REQUIRED_GOOD_SAMPLES = 3;
constexpr uint8_t MAX_CONSECUTIVE_INVALID_SAMPLES = 3;

/** 单次短路检测结果。 */
struct Result {
    uint16_t voltage_mV;   /**< 最后一次有效采样电压。 */
    uint16_t min_voltage_mV; /**< 窗口内最低有效采样电压；无有效采样时为 0。 */
    uint16_t threshold_mV; /**< 本次判定使用的阈值。 */
    uint16_t sample_count; /**< 有效采样总数。 */
    uint16_t invalid_count; /**< 无效采样总数，包含已恢复的瞬时错误。 */
    uint32_t duration_ms; /**< 检测调用耗时，包含清理；由退出守卫填写。 */
    bool     is_short;     /**< 500ms 内未获得连续三次达标采样。 */
};

/**
 * @brief 初始化组件并确保 U_SHOT_TEST 保持低电平。
 * @return ESP_OK 成功；其他值表示测试 GPIO 初始化状态异常。
 */
esp_err_t init();

/** 确认测试激励已关闭；与 test 互斥。即使旁路保护也须先清理测试激励。 */
esp_err_t ensure_idle();

/**
 * @brief 执行一次短路检测。
 *
 * 函数短时拉高 U_SHOT_TEST，经二极管和 100 欧姆电阻向输出正极施加
 * 测试电压，然后读取 U_SHOT_DETECT。退出前始终恢复测试引脚为低电平。
 * 产品代码应通过 PowerOutput::test_short_circuit 调用，由应用层确保主输出关闭
 * 且检测期间不能开启。底层 test 只提供检测互斥，不负责输出业务仲裁。
 *
 * @param result 返回测量电压、阈值和判定结果。
 * @return ESP_OK 检测完成；其他值表示 GPIO 或 ADC 操作失败。
 */
using CancelCheck = bool (*)(void* context);
/** 激励最长 500ms，间隔 10ms 采样，连续三次 >= 阈值即提前通过；有效低电压清零计数。
 * ADC 读取错误或负电压计入 invalid_count 并重试，但不打断达标计数；连续三次无效才返回检测错误。
 * 结果同时给出窗口内最低有效电压 min_voltage_mV，供失败原因展示。取消检查在采样间执行，
 * 取消返回 ESP_ERR_INVALID_STATE，并清理激励。
 */
esp_err_t test(Result& result, CancelCheck cancelled = nullptr, void* context = nullptr);

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

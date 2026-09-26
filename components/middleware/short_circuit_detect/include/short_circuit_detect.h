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

/**
 * 检测分多段：先用 FIRST_PROBE_MS 判定；若仍为低电平，则反复“断开激励
 * RELEASE_GAP_MS → 复测 CONFIRM_WINDOW_MS”，给低启动电压 / 大电容 / 低阻
 * 负载退出低阻态的机会，直到用完 MAX_TEST_TIME_MS 预算。任一段连续三次达标即
 * 判开路；全程低电平才判短路。真实短路在各段都保持低电平，负载可能在后段恢复。
 * 好负载仍在第一段提前通过，不增加等待；只有可疑时才逐步用满总预算。
 */
constexpr uint32_t MAX_TEST_TIME_MS = 3000;
constexpr uint32_t FIRST_PROBE_MS = 500;
constexpr uint32_t RELEASE_GAP_MS = 250;
constexpr uint32_t CONFIRM_WINDOW_MS = 250;
constexpr uint32_t SAMPLE_INTERVAL_MS = 10;
constexpr uint8_t REQUIRED_GOOD_SAMPLES = 3;
constexpr uint8_t MAX_CONSECUTIVE_INVALID_SAMPLES = 5;

/** 单次短路检测结果。 */
struct Result {
    uint16_t voltage_mV;   /**< 最后一次有效采样电压。 */
    uint16_t min_voltage_mV; /**< 窗口内最低有效采样电压；无有效采样时为 0。 */
    uint16_t max_voltage_mV; /**< 窗口内最高有效采样电压；无有效采样时为 0。 */
    uint16_t threshold_mV; /**< 本次判定使用的阈值。 */
    uint16_t sample_count; /**< 有效采样总数。 */
    uint16_t invalid_count; /**< 无效采样总数，包含已恢复的瞬时错误。 */
    uint32_t duration_ms; /**< 检测调用耗时，包含清理；由退出守卫填写。 */
    bool     is_short;     /**< 各段探测内均未获得连续三次达标采样。 */
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
/** 分多段探测：先跑 FIRST_PROBE_MS，遇连续三次 >= 阈值即提前通过；否则反复
 * “断开激励 RELEASE_GAP_MS → 复测 CONFIRM_WINDOW_MS”，直到用完 MAX_TEST_TIME_MS
 * 总预算，仅全程都未达标才判为短路。每段间隔 SAMPLE_INTERVAL_MS 采样。ADC 读取
 * 错误或负电压计入 invalid_count 并重试，但不打断达标计数；连续三次无效才返回
 * 检测错误。结果同时给出整个检测期间最低有效电压 min_voltage_mV，供失败原因展示。
 * 取消检查在各段采样间执行，取消返回 ESP_ERR_INVALID_STATE，并清理激励。
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

/** 短路检测结果状态。 */
enum class ResultState : uint8_t {
    NONE = 0, /**< 尚未执行过检测 */
    PASSED,   /**< 最近一次检测通过（未判定为短路） */
    FAILED,   /**< 最近一次检测判定为短路 */
};

/**
 * @brief 最近一次短路检测的结果快照，供外部查询（如 CAN 状态帧）。
 */
struct LastResult {
    ResultState state          = ResultState::NONE; /**< 结果状态 */
    int64_t     started_us     = 0;                 /**< 检测开始时的系统时间戳（esp_timer_get_time） */
    int64_t     finished_us    = 0;                 /**< 检测结束时的系统时间戳（esp_timer_get_time） */
    uint16_t    threshold_mV   = 0;                 /**< 本次判定阈值 */
    uint16_t    max_voltage_mV = 0;                 /**< 测试期间最高有效采样电压 */
    uint16_t    min_voltage_mV = 0;                 /**< 测试期间最低有效采样电压 */
    uint16_t    sample_count   = 0;                 /**< 有效采样总数 */
};

/** @return 当前是否有短路检测正在执行。 */
bool is_testing();

/** @return 最近一次短路检测的结果快照；未执行过时 state 为 NONE。 */
LastResult last_result();

} // namespace ShortCircuitDetect

#endif // SHORT_CIRCUIT_DETECT_H

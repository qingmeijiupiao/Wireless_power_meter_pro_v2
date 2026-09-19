/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 保护功能相关数据结构
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-01 18:39:46
 */
#ifndef PROTECT_H
#define PROTECT_H
#include <stdint.h>
#include <functional>
#include "esp_err.h"
enum ProtectState_t : uint8_t {
    PROTECT_STATE_NORMAL  = 0,
    PROTECT_STATE_WARNING = 1,
    PROTECT_STATE_PROTECT = 2,
};

/** @brief 单个保护通道的告警、保护和恢复阈值。 */
struct protect_threshold_t {
    float warning_threshold;
    float warning_recovery_threshold;
    float protect_threshold;
    float protect_recovery_threshold;
    uint32_t is_asc; // true: 大于等于阈值触发；false: 小于等于阈值触发。使用 4 字节保持对齐。
} __attribute__((packed));

/** @brief 四个保护通道的紧凑状态位域。 */
union protect_states_t {
    uint8_t protect_states_raw;
    struct {
        ProtectState_t temperature_protect_state  : 2;
        ProtectState_t high_voltage_protect_state : 2;
        ProtectState_t low_voltage_protect_state  : 2;
        ProtectState_t current_protect_state      : 2;
    } states_bit; // 4 个通道各占 2 bit，整体为 1 字节。
} __attribute__((packed));

/** @brief 查询接口返回的单个保护通道运行信息。 */
struct protect_channel_info_t {
    const char*         name;
    const char*         unit;
    float               now_value;
    ProtectState_t      state;
    protect_threshold_t threshold;
};

/** @brief 注册保护状态变化回调。 */
void add_on_protect_change_callback(std::function<void(ProtectState_t last_state, ProtectState_t new_state)> cb);

/** @return true 表示至少一个保护通道处于 PROTECT 状态。 */
bool have_protect();
/** @return true 表示当前存在真实保护故障。 */
bool protect_has_active_fault();
/** @return true 表示当前保护状态应阻止或关闭输出。 */
bool protect_should_block_output();
/** @brief 设置保护动作旁路状态，检测本身始终保持运行。 */
void protect_set_bypassed(bool bypassed, const char* source);
/** @return true 表示保护动作当前已旁路。 */
bool protect_is_bypassed();

/**
 * @brief 串行化保护状态/旁路写入与输出最终检查、GPIO 提交。
 * @note 只用于短事务，不能持有它执行检测、日志回调或修改保护状态。
 *       PowerOutput 锁顺序固定为输出事务锁 -> 此锁 -> GlobalState 锁。
 */
class ProtectOutputGuard {
  public:
    ProtectOutputGuard();
    ~ProtectOutputGuard();
    ProtectOutputGuard(const ProtectOutputGuard&) = delete;
    ProtectOutputGuard& operator=(const ProtectOutputGuard&) = delete;
};

/** @brief 返回保护通道数，当前固定为 4。 */
uint8_t protect_get_channel_count();

/**
 * @brief 查询指定保护通道的实时信息。
 *
 * @param index 通道编号：0=OTP，1=OVP，2=UVP，3=OCP。
 * @param info 输出通道名称、单位、当前值、状态和阈值。
 * @return true 查询成功；false 参数为空或通道不存在。
 */
bool protect_get_channel_info(uint8_t index, protect_channel_info_t* info);

/**
 * @brief 校验、保存并立即应用指定通道的保护阈值。
 *
 * @param index 通道编号：0=OTP，1=OVP，2=UVP，3=OCP。
 * @param threshold 新阈值，触发方向必须与通道原始方向一致。
 * @param source 修改来源，用于日志审计；允许传入 nullptr。
 * @return ESP_OK 设置成功；ESP_ERR_INVALID_ARG 参数或迟滞顺序非法。
 */
esp_err_t protect_set_channel_threshold(uint8_t index, const protect_threshold_t& threshold, const char* source);

/** @brief 初始化保护检测与 MOS 诊断任务。 */
esp_err_t protect_init();
/** @return true 表示保护模块已完成首次有效检测。 */
bool      protect_init_ok();
/** @brief 停止保护任务并清除运行状态。 */
esp_err_t protect_deinit();

#endif

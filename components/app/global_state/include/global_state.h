/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 该组件用于同步各种全局变量，包括保护状态、电压、电流、电能、温度等
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-25 16:24:42
 */
#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H
#include "protect.h"
#include <cstdint>
#include <type_traits>
#include <utility>

struct GlobalStateFlags {
    uint32_t output_enabled           : 1;
    uint32_t can_resistor_enabled     : 1;
    uint32_t protect_bypassed         : 1;
    uint32_t protect_initialized      : 1;
    uint32_t lp_core_running          : 1;
    uint32_t lp_ina228_initialized    : 1;
    uint32_t lp_i2c_error             : 1;
    uint32_t lp_ina228_read_timeout   : 1;
    uint32_t wifi_service_initialized : 1;
    uint32_t wifi_enabled             : 1;
    uint32_t wifi_sta_connected       : 1;
    uint32_t wifi_ap_mode             : 1;
    uint32_t wifi_has_saved_sta       : 1;
    uint32_t wifi_web_enabled_on_boot : 1;
    uint32_t web_backend_running      : 1;
    uint32_t screen_initialized       : 1;
    uint32_t blackbox_enabled         : 1;
    uint32_t reserved                 : 15;
} __attribute__((packed));
static_assert(sizeof(GlobalStateFlags) == 4, "GlobalStateFlags size must be 4 bytes");

struct GlobalState {
    GlobalStateFlags flags                = {};
    protect_states_t protect_states       = {};
    uint16_t         voltage_mV           = 0;
    int32_t          current_uA           = 0;
    float            meter_mah            = 0.0F;
    float            meter_mwh            = 0.0F;
    int16_t          board_temperature    = 0; /**< 板载温度，单位 0.01 摄氏度。 */
    int16_t          chip_temperature     = 0; /**< 芯片温度，单位 0.01 摄氏度。 */
    int16_t          current_register_raw = 0; // 兼容校准域，2.5uV/单位
    uint16_t         voltage_register_raw = 0; // 兼容诊断域，1.25mV/单位
};
static_assert(sizeof(GlobalState) == 28, "GlobalState size mismatch");

/**
 * @brief 获取全局运行状态快照
 * @return 加锁复制得到的全局运行状态快照
 */
const GlobalState get_global_state();

namespace global_state_detail {
GlobalState& unsafe_ref();
void         lock();
void         unlock();
} // namespace global_state_detail

/**
 * @brief 在全局状态锁保护下更新状态
 * @param action 可按 void(GlobalState&) 调用的轻量操作
 */
template <typename F>
void update_global_state(F&& action) {
    static_assert(std::is_invocable_r_v<void, F, GlobalState&>,
                  "update_global_state action must be callable as void(GlobalState&)");
    global_state_detail::lock();
    std::forward<F>(action)(global_state_detail::unsafe_ref());
    global_state_detail::unlock();
}

#endif

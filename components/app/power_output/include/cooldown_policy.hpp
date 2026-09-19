/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 冷却策略，上次操作后指定时间内阻止操作，开关冷却时间可分别设置
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-04-30 13:05:17
 */
#ifndef COOLDOWN_POLICY_HPP
#define COOLDOWN_POLICY_HPP

#include "power_output.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace PowerOutput {

constexpr uint32_t OUTPUT_ON_COOLDOWN_MS  = 500; /**< 开启冷却时间，OFF 操作后再次 ON 前需等待，单位毫秒 */
constexpr uint32_t OUTPUT_OFF_COOLDOWN_MS = 0;   /**< 关闭冷却时间，ON 操作后再次 OFF 前需等待，单位毫秒 */

/**
 * @brief 冷却策略，关闭后限制再次开启、开启后限制再次关闭
 * @note  OFF 操作后需等待 on_cooldown_ms 才能 ON，
 *        ON 操作后需等待 off_cooldown_ms 才能 OFF
 */
class CooldownPolicy : public OutputPolicy {
  public:
    /**
     * @brief 构造冷却策略
     * @param on_cooldown_ms  OFF->ON 操作后的冷却时间，单位毫秒
     * @param off_cooldown_ms ON->OFF 操作后的冷却时间，单位毫秒
     */
    CooldownPolicy(uint32_t on_cooldown_ms, uint32_t off_cooldown_ms)
        : _on_cooldown_ms(on_cooldown_ms), _off_cooldown_ms(off_cooldown_ms), _last_on_time_us(0),
          _last_off_time_us(0) {}

    /** @brief `check` 接口。 */
    OutputResult check(OutputOperation op, bool current_state) override {
        if (op == OutputOperation::ON) {
            // OFF 后多久才能再 ON
            return check_cooldown(_last_off_time_us, _on_cooldown_ms, "ON");
        }
        // ON 后多久才能再 OFF
        return check_cooldown(_last_on_time_us, _off_cooldown_ms, "OFF");
    }

    /** @brief `on_state_applied` 接口。 */
    void on_state_applied(OutputOperation op, bool new_state) override {
        if (op == OutputOperation::ON) {
            _last_on_time_us = esp_timer_get_time();
        } else {
            _last_off_time_us = esp_timer_get_time();
        }
    }

    /** 重置冷却计时器，允许立即操作 */
    void reset() {
        _last_on_time_us  = 0;
        _last_off_time_us = 0;
    }

  private:
    uint32_t _on_cooldown_ms;   /**< OFF->ON 冷却时间（毫秒） */
    uint32_t _off_cooldown_ms;  /**< ON->OFF 冷却时间（毫秒） */
    int64_t  _last_on_time_us;  /**< 上次 ON 操作时间（微秒），0 表示从未操作 */
    int64_t  _last_off_time_us; /**< 上次 OFF 操作时间（微秒），0 表示从未操作 */

    /** @brief `check_cooldown` 接口。 */
    OutputResult check_cooldown(int64_t last_time_us, uint32_t cooldown_ms, const char* op_name) {
        if (last_time_us == 0) {
            return OutputResult::OK;
        }
        int64_t now         = esp_timer_get_time();
        int64_t elapsed     = now - last_time_us;
        int64_t cooldown_us = (int64_t)cooldown_ms * 1000LL;
        if (elapsed < cooldown_us) {
            int64_t remaining_ms = (cooldown_us - elapsed) / 1000LL;
            ESP_LOGW("CooldownPolicy", "%s cooldown active, %lld ms remaining", op_name, remaining_ms);
            return OutputResult::FAIL_COOLDOWN_ACTIVE;
        }
        return OutputResult::OK;
    }
};

} // namespace PowerOutput

#endif

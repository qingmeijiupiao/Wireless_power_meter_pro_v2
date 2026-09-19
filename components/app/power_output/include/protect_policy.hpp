/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 保护策略，保护状态激活时阻止开启输出
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-04-30
 */
#ifndef PROTECT_POLICY_HPP
#define PROTECT_POLICY_HPP

#include "power_output.h"
#include "protect.h"
#include "esp_log.h"

namespace PowerOutput {

/**
 * @brief 保护策略，在保护状态激活时阻止开启输出
 * @note  仅在 ON 操作时检查，OFF 操作始终允许
 */
class ProtectPolicy : public OutputPolicy {
  public:
    /** @brief `check` 接口。 */
    OutputResult check(OutputOperation op, bool current_state) override {
        if (op == OutputOperation::ON && protect_should_block_output()) {
            return OutputResult::FAIL_PROTECT_ACTIVE;
        }
        return OutputResult::OK;
    }

    /** @brief `on_state_applied` 接口。 */
    void on_state_applied(OutputOperation op, bool new_state) override {}
};

} // namespace PowerOutput

#endif

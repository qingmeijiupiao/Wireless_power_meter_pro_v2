#pragma once
#include "power_output.h"
#include "widgets/output_view.h"
#include <cstdio>
#include <cstring>

namespace SCREEN {
/** 将真实输出状态和操作结果映射为显示数据，不执行任何输出操作。
 * 使用无符号经过时间，支持毫秒计数回绕；短提示只在新结果到达时开始计时。
 */
class OutputFeedback {
  public:
    UI::OutputView update(const PowerOutput::Status& status, uint32_t now, bool pressed) {
        using R = PowerOutput::OutputResult;
        using V = UI::OutputVisual;
        if (status.request_id != request_id_ || status.result != result_) {
            request_id_ = status.request_id;
            result_ = status.result;
            received_ms_ = now;
        }
        const bool recent = request_id_ != 0 && now - received_ms_ < NOTICE_MS;
        UI::OutputView view;
        view.pressed = pressed;
        view.bypassed = status.bypassed;
        view.animation = status.checking ? (now / 100) % 3 : 0;
        const char* label = "OFF";
        const char* detail = "";
        if (status.output_on) {
            view.state = V::On;
            label = "ON";
        } else if (!status.initialized) {
            view.state = V::Wait;
            label = "INIT";
        } else if (status.checking) {
            view.state = V::Checking;
            label = "CHECK";
        } else if (!status.bypassed && status.protect_mask) {
            view.state = V::Locked;
            label = "LOCK";
        } else if (recent && result_ == R::FAIL_COOLDOWN_ACTIVE && status.cooldown_remaining_ms) {
            // 只有冷却期内尝试开启被拒时才提示等待；普通关闭直接显示 OFF。
            view.state = V::Wait;
            label = "WAIT";
        } else if (recent && result_ != R::OK && result_ != R::PENDING &&
                   result_ != R::FAIL_CANCELLED && result_ != R::FAIL_COOLDOWN_ACTIVE) {
            view.state = V::Error;
            label = "ERR";
        }
        if (recent) {
            switch (result_) {
            case R::FAIL_PROTECT_ACTIVE:
                detail = status.protect_mask & 1 ? "OTP ACTIVE" : status.protect_mask & 2 ? "OVP ACTIVE" :
                         status.protect_mask & 4 ? "UVP ACTIVE" : status.protect_mask & 8 ? "OCP ACTIVE" : "PROTECTED";
                break;
            case R::FAIL_COOLDOWN_ACTIVE:
                if (status.cooldown_remaining_ms) {
                    const auto tenths = (status.cooldown_remaining_ms + 99) / 100;
                    std::snprintf(view.detail, sizeof(view.detail), "WAIT %lu.%lus",
                                  static_cast<unsigned long>(tenths / 10), static_cast<unsigned long>(tenths % 10));
                }
                break;
            case R::FAIL_BUSY: detail = "OUTPUT BUSY"; break;
            case R::FAIL_NOT_INIT: detail = "NOT READY"; break;
            case R::FAIL_CANCELLED: detail = "CANCELLED"; break;
            case R::FAIL_GPIO: detail = "OUTPUT ERROR"; break;
            case R::FAIL_TIMEOUT: detail = "CHECK TIMEOUT"; break;
            case R::FAIL_SHORT_CIRCUIT: detail = "SHORT CIRCUIT"; break;
            case R::FAIL_SHORT_DETECT: detail = "CHECK FAILED"; break;
            default: break;
            }
        }
        if (*detail) std::snprintf(view.detail, sizeof(view.detail), "%s", detail);
        std::snprintf(view.label, sizeof(view.label), "%s", label);
        const bool cooldown_wait = recent && result_ == R::FAIL_COOLDOWN_ACTIVE && status.cooldown_remaining_ms;
        next_ms_ = status.checking || cooldown_wait ? 100 - now % 100 : UINT32_MAX;
        if (recent && result_ != R::OK && result_ != R::PENDING) {
            const uint32_t remaining = NOTICE_MS - (now - received_ms_);
            if (remaining < next_ms_) next_ms_ = remaining;
        }
        return view;
    }
    uint32_t next_update_ms() const { return next_ms_; }
    static bool equal(const UI::OutputView& a, const UI::OutputView& b) {
        return a.state == b.state && a.pressed == b.pressed && a.bypassed == b.bypassed &&
               a.animation == b.animation && std::strcmp(a.label, b.label) == 0 &&
               std::strcmp(a.detail, b.detail) == 0;
    }
  private:
    static constexpr uint32_t NOTICE_MS = 2000;
    uint32_t request_id_ = 0, received_ms_ = 0, next_ms_ = UINT32_MAX;
    PowerOutput::OutputResult result_ = PowerOutput::OutputResult::OK;
};
} // namespace SCREEN

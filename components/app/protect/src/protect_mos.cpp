#include "protect_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "diagnostic_log.h"
#include "esp_log.h"
#include <cstdlib>

namespace {

constexpr char TAG[] = "protect";

// MOS 诊断与主保护任务解耦，低频运行，避免输出切换和电机瞬态造成误报。
constexpr TickType_t MOS_CHECK_INTERVAL_TICKS  = pdMS_TO_TICKS(250);
// 输出关闭后 INA228 平均窗口、电机惯性和输出电容都可能产生残留电流，先等待稳定。
constexpr TickType_t MOS_OUTPUT_SETTLE_TICKS   = pdMS_TO_TICKS(3000);
// 达到该持续时间只记录 suspicious，用于现场排查，不直接判定硬件损坏。
constexpr TickType_t MOS_SUSPICIOUS_TICKS      = pdMS_TO_TICKS(2000);
// 达到该持续时间才上报 suspected fault，仍然只做诊断日志，不参与保护关断。
constexpr TickType_t MOS_FAULT_TICKS           = pdMS_TO_TICKS(5000);
// 关断尾流与采样偏移可能造成误判，因此提高阈值以过滤关断尾流。
constexpr int32_t    MOS_SUSPICIOUS_CURRENT_UA = 100 * 1000;

TaskHandle_t mos_task_handle = nullptr;

/**
 * @brief MOS 损坏后台诊断任务。
 *
 * @note 只有在输出关闭足够久、INA228 测量可靠且异常电流持续存在时才记录诊断事件。
 *       INA228 降级时立即放弃当前检测窗口，避免用陈旧或无效电流误报 MOS 损坏。
 */
void mos_fault_task(void*) {
    TickType_t ticks                  = xTaskGetTickCount();
    // 记录输出进入关闭状态的时刻，用于避开输出关断后的瞬态区间。
    TickType_t output_off_start_ticks = 0;
    // 记录异常电流连续存在的起点，用于分级上报 suspicious / suspected fault。
    TickType_t detection_start_ticks  = 0;
    bool       output_was_enabled     = true;
    bool       detection_active       = false;
    bool       suspicious_reported    = false;
    bool       fault_reported         = false;
    bool       unreliable_reported    = false;

    while (true) {
        const TickType_t now_ticks = xTaskGetTickCount();
        const auto       state     = get_global_state();

        if (state.flags.output_enabled) {
            output_was_enabled  = true;
            detection_active    = false;
            suspicious_reported = false;
            fault_reported      = false;
            unreliable_reported = false;
            xTaskDelayUntil(&ticks, MOS_CHECK_INTERVAL_TICKS);
            continue;
        }

        if (output_was_enabled) {
            output_was_enabled     = false;
            output_off_start_ticks = now_ticks;
            detection_active       = false;
            suspicious_reported    = false;
            fault_reported         = false;
            unreliable_reported    = false;
            xTaskDelayUntil(&ticks, MOS_CHECK_INTERVAL_TICKS);
            continue;
        }

        if (now_ticks - output_off_start_ticks < MOS_OUTPUT_SETTLE_TICKS) {
            xTaskDelayUntil(&ticks, MOS_CHECK_INTERVAL_TICKS);
            continue;
        }

        if (!protect_ina228_measurement_reliable()) {
            // 测量链路不可靠时不保留检测窗口；恢复后必须重新累计持续时间。
            detection_active    = false;
            suspicious_reported = false;
            fault_reported      = false;
            if (!unreliable_reported) {
                DEVICE_EVENT_W(TAG, "mos: skipped reason=ina228_unreliable");
                unreliable_reported = true;
            }
            xTaskDelayUntil(&ticks, MOS_CHECK_INTERVAL_TICKS);
            continue;
        }
        unreliable_reported = false;

        const int32_t current_uA = std::abs(state.current_uA);
        if (current_uA < MOS_SUSPICIOUS_CURRENT_UA) {
            // 电流回落后清除窗口，下一次异常必须重新持续满确认时间。
            detection_active    = false;
            suspicious_reported = false;
            fault_reported      = false;
            xTaskDelayUntil(&ticks, MOS_CHECK_INTERVAL_TICKS);
            continue;
        }

        if (!detection_active) {
            detection_active      = true;
            detection_start_ticks = now_ticks;
            xTaskDelayUntil(&ticks, MOS_CHECK_INTERVAL_TICKS);
            continue;
        }

        const TickType_t detection_ticks = now_ticks - detection_start_ticks;
        if (!suspicious_reported && detection_ticks >= MOS_SUSPICIOUS_TICKS) {
            DEVICE_EVENT_W(TAG, "mos: suspicious current_ma=%ld duration_ms=%lu", static_cast<int32_t>(current_uA / 1000),
                           static_cast<uint32_t>(detection_ticks * portTICK_PERIOD_MS));
            suspicious_reported = true;
        }

        if (!fault_reported && detection_ticks >= MOS_FAULT_TICKS) {
            ESP_LOGE(TAG, "MOS fault suspected: output is off but current remains, current_ma=%ld duration_ms=%lu",
                     static_cast<int32_t>(current_uA / 1000),
                     static_cast<uint32_t>(detection_ticks * portTICK_PERIOD_MS));
            fault_reported = true;
        }

        xTaskDelayUntil(&ticks, MOS_CHECK_INTERVAL_TICKS);
    }
}

} // namespace

/**
 * @brief 创建 MOS 诊断任务。
 */
void protect_mos_fault_start() {
    if (mos_task_handle != nullptr) {
        return;
    }
    constexpr uint32_t mos_task_stack_size = 2304;
    if (xTaskCreate(mos_fault_task, "protect_mos", mos_task_stack_size, nullptr, 4, &mos_task_handle) != pdPASS) {
        mos_task_handle = nullptr;
        ESP_LOGE(TAG, "failed to create MOS diagnostic task");
    }
}

/**
 * @brief 删除 MOS 诊断任务并清空任务句柄。
 */
void protect_mos_fault_stop() {
    if (mos_task_handle == nullptr) {
        return;
    }
    vTaskDelete(mos_task_handle);
    mos_task_handle = nullptr;
}

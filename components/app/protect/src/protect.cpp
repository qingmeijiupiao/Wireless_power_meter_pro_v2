#include "protect.h"
#include "protect_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "diagnostic_log.h"
#include "HXC_NVS.h"
#include <bit>
#include <cmath>
#include <vector>
#ifndef ENABLE_PROTECT_LOG
#define ENABLE_PROTECT_LOG 1
#endif

#if ENABLE_PROTECT_LOG == 1
#include "esp_log.h"
#define PROTECT_LOG_TAG "protect"
#define PROTECT_LOGE(fmt, ...) ESP_LOGE(PROTECT_LOG_TAG, fmt, ##__VA_ARGS__)
#define PROTECT_LOGW(fmt, ...) ESP_LOGW(PROTECT_LOG_TAG, fmt, ##__VA_ARGS__)
#define PROTECT_LOGI(fmt, ...) ESP_LOGI(PROTECT_LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define PROTECT_LOGE(fmt, ...) (void)
#define PROTECT_LOGW(fmt, ...) (void)
#define PROTECT_LOGI(fmt, ...) (void)
#endif

TaskHandle_t protect_task_handle = nullptr;

/** @brief 将物理量转换为千分之一单位，供日志使用整数格式输出。 */
static int32_t to_milli(float value) {
    return static_cast<int32_t>(value * 1000.0f);
}

static bool ina228_measurement_reliable(const GlobalStateFlags& flags) {
    return flags.lp_ina228_initialized && !flags.lp_i2c_error && !flags.lp_ina228_read_timeout;
}

/**
 * @brief 将已正式提交的保护状态变化写入黑匣子。
 *
 * 日志同时保留当前值、主要阈值、旁路状态、输出状态和 INA228 原始值，
 * 用于离线定位保护触发原因。
 */
static void log_state_change_event(const char* channel, ProtectState_t last_state, ProtectState_t new_state,
                                   float value, const protect_threshold_t& threshold) {
    const auto     state       = get_global_state();
    const int32_t  current_raw = state.current_register_raw;
    const uint32_t voltage_raw = state.voltage_register_raw;
    if (new_state == PROTECT_STATE_NORMAL) {
        DEVICE_STATE_I(PROTECT_LOG_TAG,
                       "protect: state channel=%s old=%u new=%u value_milli=%ld warn=%ld protect=%ld bypass=%u "
                       "output=%u raw_i=%d raw_v=%u",
                       channel, static_cast<uint32_t>(last_state), static_cast<uint32_t>(new_state),
                       static_cast<int32_t>(to_milli(value)), static_cast<int32_t>(to_milli(threshold.warning_threshold)),
                       static_cast<int32_t>(to_milli(threshold.protect_threshold)),
                       state.flags.protect_bypassed ? 1U : 0U,
                       state.flags.output_enabled ? 1U : 0U, current_raw, voltage_raw);
    } else {
        DEVICE_STATE_W(PROTECT_LOG_TAG,
                       "protect: state channel=%s old=%u new=%u value_milli=%ld warn=%ld protect=%ld bypass=%u "
                       "output=%u raw_i=%d raw_v=%u",
                       channel, static_cast<uint32_t>(last_state), static_cast<uint32_t>(new_state),
                       static_cast<int32_t>(to_milli(value)), static_cast<int32_t>(to_milli(threshold.warning_threshold)),
                       static_cast<int32_t>(to_milli(threshold.protect_threshold)),
                       state.flags.protect_bypassed ? 1U : 0U,
                       state.flags.output_enabled ? 1U : 0U, current_raw, voltage_raw);
    }
}

// 以下运行期阈值会在首次访问时由 NVS 配置覆盖；声明值与默认配置保持一致。
protect_threshold_t temperature_threshold = {
    .warning_threshold          = 60.0f,
    .warning_recovery_threshold = 55.0f,
    .protect_threshold          = 80.0f,
    .protect_recovery_threshold = 75.0f,
    .is_asc                     = true,
};

protect_threshold_t high_voltage_threshold = {
    .warning_threshold          = 25.5f,
    .warning_recovery_threshold = 25.3f,
    .protect_threshold          = 27.5f,
    .protect_recovery_threshold = 27.0f,
    .is_asc                     = true,
};

protect_threshold_t low_voltage_threshold = {
    .warning_threshold          = 6.6f,
    .warning_recovery_threshold = 7.2f,
    .protect_threshold          = 4.7f,
    .protect_recovery_threshold = 5.0f,
    .is_asc                     = false,
};

protect_threshold_t current_threshold = {
    .warning_threshold          = 15.0f,
    .warning_recovery_threshold = 15.0f,
    .protect_threshold          = 25.0f,
    .protect_recovery_threshold = 25.00f,
    .is_asc                     = true,
};

/** @brief 四个保护通道的完整持久化配置。 */
struct protect_config_t {
    protect_threshold_t temperature;
    protect_threshold_t high_voltage;
    protect_threshold_t low_voltage;
    protect_threshold_t current;
};

// NVS 中没有有效数据时使用的出厂默认值。
static constexpr protect_config_t DEFAULT_PROTECT_CONFIG = {
    .temperature =
        {
            .warning_threshold          = 60.0f,
            .warning_recovery_threshold = 55.0f,
            .protect_threshold          = 80.0f,
            .protect_recovery_threshold = 75.0f,
            .is_asc                     = true,
        },
    .high_voltage =
        {
            .warning_threshold          = 25.5f,
            .warning_recovery_threshold = 25.3f,
            .protect_threshold          = 27.5f,
            .protect_recovery_threshold = 27.0f,
            .is_asc                     = true,
        },
    .low_voltage =
        {
            .warning_threshold          = 6.6f,
            .warning_recovery_threshold = 7.2f,
            .protect_threshold          = 4.7f,
            .protect_recovery_threshold = 5.0f,
            .is_asc                     = false,
        },
    .current =
        {
            .warning_threshold          = 15.0f,
            .warning_recovery_threshold = 15.0f,
            .protect_threshold          = 25.0f,
            .protect_recovery_threshold = 25.0f,
            .is_asc                     = true,
        },
};

// 四组阈值整体保存，避免逐字段更新造成通道配置不一致。
static HXC::NVS_DATA<protect_config_t> protect_config_data("PROTECT_CFG", DEFAULT_PROTECT_CONFIG);
static bool                            protect_config_loaded = false;

/**
 * @brief 校验单个保护通道的阈值顺序和数值范围。
 *
 * 升序通道要求：告警恢复 <= 告警 <= 保护恢复 <= 保护。
 * 降序通道要求：保护 <= 保护恢复 <= 告警 <= 告警恢复。
 */
static bool validate_threshold(const protect_threshold_t& threshold) {
    if (!std::isfinite(threshold.warning_threshold) || !std::isfinite(threshold.warning_recovery_threshold) ||
        !std::isfinite(threshold.protect_threshold) || !std::isfinite(threshold.protect_recovery_threshold) ||
        threshold.warning_threshold < 0.0f || threshold.warning_recovery_threshold < 0.0f ||
        threshold.protect_threshold < 0.0f || threshold.protect_recovery_threshold < 0.0f) {
        return false;
    }

    if (threshold.is_asc) {
        return threshold.warning_recovery_threshold <= threshold.warning_threshold &&
               threshold.warning_threshold <= threshold.protect_recovery_threshold &&
               threshold.protect_recovery_threshold <= threshold.protect_threshold;
    }
    return threshold.protect_threshold <= threshold.protect_recovery_threshold &&
           threshold.protect_recovery_threshold <= threshold.warning_threshold &&
           threshold.warning_threshold <= threshold.warning_recovery_threshold;
}

/** @brief 将一组配置写入运行期阈值变量。 */
static void apply_protect_config(const protect_config_t& config) {
    temperature_threshold  = config.temperature;
    high_voltage_threshold = config.high_voltage;
    low_voltage_threshold  = config.low_voltage;
    current_threshold      = config.current;
}

/** @brief 校验保护配置，避免损坏的 NVS 数据进入运行期。 */
static bool validate_protect_config(const protect_config_t& config) {
    return config.temperature.is_asc == 1 && config.high_voltage.is_asc == 1 && config.low_voltage.is_asc == 0 &&
           config.current.is_asc == 1 && validate_threshold(config.temperature) &&
           validate_threshold(config.high_voltage) && validate_threshold(config.low_voltage) &&
           validate_threshold(config.current);
}

/** @brief 首次使用时从 NVS 加载保护阈值，非法配置回退为默认值。 */
static void ensure_protect_config_loaded() {
    if (protect_config_loaded) {
        return;
    }

    protect_config_t config = protect_config_data.read();
    if (!validate_protect_config(config)) {
        PROTECT_LOGE("invalid protect thresholds in NVS, restoring defaults");
        config                   = DEFAULT_PROTECT_CONFIG;
        const esp_err_t save_err = protect_config_data.set(config);
        if (save_err != ESP_OK) {
            PROTECT_LOGE("failed to restore default thresholds: %s", esp_err_to_name(save_err));
        }
    }
    apply_protect_config(config);
    protect_config_loaded = true;
}

/** @brief 分行输出并持久化记录单个通道的阈值，避免单条日志过长。 */
static void log_threshold_values(const char* channel, const protect_threshold_t& threshold) {
    DEVICE_EVENT_I(PROTECT_LOG_TAG, "protect: threshold channel=%s warn_milli=%ld warn_rec_milli=%ld", channel,
                   static_cast<int32_t>(to_milli(threshold.warning_threshold)),
                   static_cast<int32_t>(to_milli(threshold.warning_recovery_threshold)));
    DEVICE_EVENT_I(PROTECT_LOG_TAG, "protect: threshold channel=%s protect_milli=%ld protect_rec_milli=%ld", channel,
                   static_cast<int32_t>(to_milli(threshold.protect_threshold)),
                   static_cast<int32_t>(to_milli(threshold.protect_recovery_threshold)));
}

/** @brief 输出并持久化记录保护模块启动时实际使用的阈值。 */
static void log_initial_thresholds() {
    for (uint8_t i = 0; i < protect_get_channel_count(); ++i) {
        protect_channel_info_t info = {};
        if (protect_get_channel_info(i, &info)) {
            log_threshold_values(info.name, info.threshold);
        }
    }
}

/**
 * @brief 根据当前值计算通道候选状态。
 *
 * @note 本函数只计算候选状态，不直接写回全局状态。候选状态还需要经过
 *       debounce_protect_state() 的固定时间迟滞后才能正式提交。
 */
ProtectState_t check_now_state(protect_threshold_t threshold, ProtectState_t last_state, float now_value) {
    // 用lambda统一处理升序/降序的阈值比较
    auto is_triggered = [&](float value, float th) -> bool { return threshold.is_asc ? (value >= th) : (value <= th); };

    switch (last_state) {
    case PROTECT_STATE_NORMAL:
        // 直接越过保护阈值
        if (is_triggered(now_value, threshold.protect_threshold)) {
            return PROTECT_STATE_PROTECT;
        }
        // 越过警告阈值
        if (is_triggered(now_value, threshold.warning_threshold)) {
            return PROTECT_STATE_WARNING;
        }
        return PROTECT_STATE_NORMAL;

    case PROTECT_STATE_WARNING:
        // 继续恶化至保护阈值
        if (is_triggered(now_value, threshold.protect_threshold)) {
            return PROTECT_STATE_PROTECT;
        }
        // 恢复到警告恢复阈值以下（利用反向比较）
        if (!is_triggered(now_value, threshold.warning_recovery_threshold)) {
            return PROTECT_STATE_NORMAL;
        }
        return PROTECT_STATE_WARNING;

    case PROTECT_STATE_PROTECT:
        // 恢复到保护恢复阈值以下，退回警告状态（而非直接正常，需二次确认）
        if (!is_triggered(now_value, threshold.protect_recovery_threshold)) {
            return PROTECT_STATE_WARNING;
        }
        return PROTECT_STATE_PROTECT;

    default:
        PROTECT_LOGE("unknown state %d", last_state);
        return PROTECT_STATE_NORMAL;
    }
}

// OTP 保持较短触发确认；INA228 相关通道使用更长确认时间过滤电机和总线瞬态。
constexpr uint32_t protect_state_change_delay_ms        = 200;
constexpr uint32_t ina228_protect_state_change_delay_ms = 2000;

/**
 * @brief 返回指定保护通道的触发确认时间。
 *
 * @param channel 保护通道编号：0=OTP，1=OVP，2=UVP，3=OCP。

 * *
 * @return 候选状态进入更严重级别前必须持续的 FreeRTOS tick 数。
 *
 * @note INA228
 * 相关通道面对电机反电动势、I2C 瞬断和机械振动时更容易出现短时尖峰，
 *       因此使用 2s
 * 触发确认；恢复路径不调用该时间，恢复立即生效。

 */
static TickType_t protect_state_change_delay_ticks(uint8_t channel) {
    return pdMS_TO_TICKS(protect_is_ina228_channel(channel) ? ina228_protect_state_change_delay_ms
                                                            : protect_state_change_delay_ms);
}

/** @brief 单个通道正在等待提交的候选状态及其起始时间。 */
struct protect_pending_state_t {
    ProtectState_t state;
    TickType_t     start_ticks;
    bool           active;
};

// OTP、OVP、UVP、OCP 四个通道分别维护独立计时，互不影响。
static protect_pending_state_t protect_pending_states[4] = {};

/** @brief 清除所有通道尚未提交的状态切换计时。 */
static void reset_pending_protect_states() {
    for (auto& pending : protect_pending_states) {
        pending = {};
    }
}

/**
 * @brief 对保护状态恶化执行按通道区分的触发确认。
 *
 *
 * 状态进入更严重级别时，候选状态必须连续保持满通道对应的触发确认时间后才正式提交。

 * * 候选状态恢复为当前状态，或在等待期间变化为另一状态时，原计时立即取消。
 *
 *
 * @note 恢复到更轻状态不做时间迟滞，保证故障解除或 INA228
 * 降级解除后不会继续阻塞调试输出。

 */
static ProtectState_t debounce_protect_state(uint8_t channel, ProtectState_t current_state,
                                             ProtectState_t candidate_state) {
    if (channel >= sizeof(protect_pending_states) / sizeof(protect_pending_states[0])) {
        return current_state;
    }
    protect_pending_state_t& pending = protect_pending_states[channel];
    if (candidate_state == current_state) {
        pending.active = false;
        return current_state;
    }

    // 恢复不做时间迟滞，避免保护解除后继续影响调试操作。
    if (candidate_state < current_state) {
        pending.active = false;
        return candidate_state;
    }

    const TickType_t now_ticks = xTaskGetTickCount();
    if (!pending.active || pending.state != candidate_state) {
        pending.state       = candidate_state;
        pending.start_ticks = now_ticks;
        pending.active      = true;
        return current_state;
    }

    const TickType_t delay_ticks = protect_state_change_delay_ticks(channel);
    if (now_ticks - pending.start_ticks < delay_ticks) {
        return current_state;
    }

    pending.active = false;
    PROTECT_LOGI("state change channel=%u state=%u->%u delay_ms=%lu", static_cast<uint32_t>(channel),
                 static_cast<uint32_t>(current_state), static_cast<uint32_t>(candidate_state),
                 static_cast<uint32_t>(delay_ticks * portTICK_PERIOD_MS));
    return candidate_state;
}

static std::vector<std::function<void(ProtectState_t, ProtectState_t)>> protect_change_callbacks;

void add_on_protect_change_callback(std::function<void(ProtectState_t last_state, ProtectState_t new_state)> cb) {
    protect_change_callbacks.push_back(cb);
}

bool have_protect() {
    return protect_has_active_fault();
}

bool protect_has_active_fault() {
    const auto  state                 = get_global_state();
    const auto& global_state_protects = state.protect_states.states_bit;
    return global_state_protects.temperature_protect_state == PROTECT_STATE_PROTECT ||
           global_state_protects.high_voltage_protect_state == PROTECT_STATE_PROTECT ||
           global_state_protects.low_voltage_protect_state == PROTECT_STATE_PROTECT ||
           global_state_protects.current_protect_state == PROTECT_STATE_PROTECT;
}

void protect_set_bypassed(bool bypassed, const char* source) {
    source = source == nullptr ? "unknown" : source;
    bool changed      = false;
    bool output_on    = false;
    bool active_fault = false;
    update_global_state([&](GlobalState& state) {
        if (state.flags.protect_bypassed == bypassed) {
            return;
        }
        auto& protects = state.protect_states.states_bit;
        active_fault = protects.temperature_protect_state == PROTECT_STATE_PROTECT ||
                       protects.high_voltage_protect_state == PROTECT_STATE_PROTECT ||
                       protects.low_voltage_protect_state == PROTECT_STATE_PROTECT ||
                       protects.current_protect_state == PROTECT_STATE_PROTECT;
        output_on                       = state.flags.output_enabled;
        state.flags.protect_bypassed    = bypassed;
        changed                         = true;
    });
    if (!changed) {
        return;
    }
    DEVICE_STATE_W(PROTECT_LOG_TAG, "protect: bypass source=%s old=%u new=%u active_fault=%u output=%u", source,
                   bypassed ? 0U : 1U, bypassed ? 1U : 0U, active_fault ? 1U : 0U, output_on ? 1U : 0U);
}

bool protect_is_bypassed() {
    return get_global_state().flags.protect_bypassed;
}

bool protect_should_block_output() {
    const auto state = get_global_state();
    if (state.flags.protect_bypassed) {
        return false;
    }
    const auto& states = state.protect_states.states_bit;
    if (states.temperature_protect_state == PROTECT_STATE_PROTECT) {
        return true;
    }
    // INA228 降级时不允许 OVP/UVP/OCP 的旧状态继续阻止输出，避免传感器异常影响机器人调试。
    if (!ina228_measurement_reliable(state.flags)) {
        return false;
    }
    return states.high_voltage_protect_state == PROTECT_STATE_PROTECT ||
           states.low_voltage_protect_state == PROTECT_STATE_PROTECT ||
           states.current_protect_state == PROTECT_STATE_PROTECT;
}

/**
 * @brief 判断 INA228 测量链路是否可用于保护决策。
 *
 * @return true 当前电压/电流数据可以参与 OVP、UVP、OCP 和
 * MOS 诊断；false 当前只能展示/记录降级状态。
 */
bool protect_ina228_measurement_reliable() {
    return ina228_measurement_reliable(get_global_state().flags);
}

/**
 * @brief 判断通道是否依赖 INA228 电压/电流数据。
 */
bool protect_is_ina228_channel(uint32_t channel) {
    return channel == 1 || channel == 2 || channel == 3;
}

uint8_t protect_get_channel_count() {
    return 4;
}

bool protect_get_channel_info(uint8_t index, protect_channel_info_t* info) {
    if (info == nullptr) {
        return false;
    }
    ensure_protect_config_loaded();

    const auto  state                 = get_global_state();
    const auto& global_state_protects = state.protect_states.states_bit;
    switch (index) {
    case 0:
        *info = {
            .name      = "OTP",
            .unit      = "C",
            .now_value = state.board_temperature / 100.0f,
            .state     = global_state_protects.temperature_protect_state,
            .threshold = temperature_threshold,
        };
        return true;
    case 1:
        *info = {
            .name      = "OVP",
            .unit      = "V",
            .now_value = state.voltage_mV / 1000.0f,
            .state     = global_state_protects.high_voltage_protect_state,
            .threshold = high_voltage_threshold,
        };
        return true;
    case 2:
        *info = {
            .name      = "UVP",
            .unit      = "V",
            .now_value = state.voltage_mV / 1000.0f,
            .state     = global_state_protects.low_voltage_protect_state,
            .threshold = low_voltage_threshold,
        };
        return true;
    case 3:
        *info = {
            .name      = "OCP",
            .unit      = "A",
            .now_value = std::abs(state.current_uA) / 1000000.0f,
            .state     = global_state_protects.current_protect_state,
            .threshold = current_threshold,
        };
        return true;
    default:
        return false;
    }
}

esp_err_t protect_set_channel_threshold(uint8_t index, const protect_threshold_t& threshold, const char* source) {
    ensure_protect_config_loaded();
    protect_channel_info_t info = {};
    if (!protect_get_channel_info(index, &info) || threshold.is_asc != info.threshold.is_asc ||
        !validate_threshold(threshold)) {
        return ESP_ERR_INVALID_ARG;
    }

    // 复制完整配置，仅替换目标通道，确保单次 NVS 写入得到一致快照。
    protect_config_t config = {
        .temperature  = temperature_threshold,
        .high_voltage = high_voltage_threshold,
        .low_voltage  = low_voltage_threshold,
        .current      = current_threshold,
    };
    protect_threshold_t* target = nullptr;
    switch (index) {
    case 0:
        target = &config.temperature;
        break;
    case 1:
        target = &config.high_voltage;
        break;
    case 2:
        target = &config.low_voltage;
        break;
    case 3:
        target = &config.current;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    *target = threshold;

    const esp_err_t save_err = protect_config_data.set(config);
    if (save_err != ESP_OK) {
        PROTECT_LOGE("failed to persist thresholds: %s", esp_err_to_name(save_err));
        return save_err;
    }
    apply_protect_config(config);
    source = source == nullptr ? "unknown" : source;
    DEVICE_EVENT_I(PROTECT_LOG_TAG, "protect: threshold_updated source=%s channel=%s", source, info.name);
    log_threshold_values(info.name, threshold);
    return ESP_OK;
}

static bool _protect_init_ok = false;

/**
 * @brief 保护轮询任务。
 *
 * 以 20Hz 依次检查 OTP、OVP、UVP、OCP。每个通道先计算候选状态，状态恶化时再执行

 * *
 * 按通道区分的触发确认；恢复状态立即提交。正式切换后记录黑匣子并通知输出控制等订阅模块。
 *
 * @note INA228
 * 不可靠时 OVP、UVP、OCP 直接恢复为 NORMAL，不参与输出阻断。
 */
void protect_task(void* pvParameters) {
    auto           ticks                 = xTaskGetTickCount();
    constexpr int  protect_check_HZ      = 20;
    ProtectState_t temp_state;
    ProtectState_t last_state;
    static bool    first_check          = true;
    bool           last_ina228_reliable = protect_ina228_measurement_reliable();

    while (1) {
        const auto state           = get_global_state();
        auto       global_state_protects = state.protect_states.states_bit;
        const bool ina228_reliable = ina228_measurement_reliable(state.flags);
        if (ina228_reliable != last_ina228_reliable) {
            if (ina228_reliable) {
                DEVICE_STATE_I(PROTECT_LOG_TAG, "protect: measurement old=unreliable new=reliable");
            } else {
                DEVICE_STATE_W(PROTECT_LOG_TAG,
                               "protect: measurement old=reliable new=unreliable reason=ina228 flags=0x%08lx",
                               static_cast<uint32_t>(std::bit_cast<uint32_t>(state.flags)));
            }
            last_ina228_reliable = ina228_reliable;
        }

        // 检查温度保护状态
        temp_state = debounce_protect_state(0, global_state_protects.temperature_protect_state,
                                            check_now_state(temperature_threshold,
                                                            global_state_protects.temperature_protect_state,
                                                            state.board_temperature / 100.0f));
        if (temp_state != global_state_protects.temperature_protect_state) {
            last_state                                      = global_state_protects.temperature_protect_state;
            global_state_protects.temperature_protect_state = temp_state;
            update_global_state([temp_state](GlobalState& state) {
                state.protect_states.states_bit.temperature_protect_state = temp_state;
            });
            log_state_change_event("OTP", last_state, temp_state, state.board_temperature / 100.0f,
                                   temperature_threshold);
            for (auto& cb : protect_change_callbacks) {
                cb(last_state, temp_state);
            }
        }

        // 检查电压保护状态
        temp_state = debounce_protect_state(
            1, global_state_protects.high_voltage_protect_state,
            ina228_reliable ? check_now_state(high_voltage_threshold, global_state_protects.high_voltage_protect_state,
                                              state.voltage_mV / 1e3)
                            : PROTECT_STATE_NORMAL);
        if (temp_state != global_state_protects.high_voltage_protect_state) {
            last_state                                       = global_state_protects.high_voltage_protect_state;
            global_state_protects.high_voltage_protect_state = temp_state;
            update_global_state([temp_state](GlobalState& state) {
                state.protect_states.states_bit.high_voltage_protect_state = temp_state;
            });
            log_state_change_event("OVP", last_state, temp_state, state.voltage_mV / 1e3, high_voltage_threshold);
            for (auto& cb : protect_change_callbacks) {
                cb(last_state, temp_state);
            }
        }

        temp_state = debounce_protect_state(
            2, global_state_protects.low_voltage_protect_state,
            ina228_reliable ? check_now_state(low_voltage_threshold, global_state_protects.low_voltage_protect_state,
                                              state.voltage_mV / 1e3)
                            : PROTECT_STATE_NORMAL);
        if (temp_state != global_state_protects.low_voltage_protect_state) {
            last_state                                      = global_state_protects.low_voltage_protect_state;
            global_state_protects.low_voltage_protect_state = temp_state;
            update_global_state([temp_state](GlobalState& state) {
                state.protect_states.states_bit.low_voltage_protect_state = temp_state;
            });
            log_state_change_event("UVP", last_state, temp_state, state.voltage_mV / 1e3, low_voltage_threshold);
            for (auto& cb : protect_change_callbacks) {
                cb(last_state, temp_state);
            }
        }

        // 检查电流保护状态
        temp_state = debounce_protect_state(
            3, global_state_protects.current_protect_state,
            ina228_reliable ? check_now_state(current_threshold, global_state_protects.current_protect_state,
                                              std::abs(state.current_uA) / 1e6)
                            : PROTECT_STATE_NORMAL);
        if (temp_state != global_state_protects.current_protect_state) {
            last_state                                  = global_state_protects.current_protect_state;
            global_state_protects.current_protect_state = temp_state;
            update_global_state([temp_state](GlobalState& state) {
                state.protect_states.states_bit.current_protect_state = temp_state;
            });
            log_state_change_event("OCP", last_state, temp_state, std::abs(state.current_uA) / 1e6,
                                   current_threshold);
            for (auto& cb : protect_change_callbacks) {
                cb(last_state, temp_state);
            }
        }

        if (first_check) {
            first_check                               = false;
            _protect_init_ok                          = true;
            update_global_state([](GlobalState& state) { state.flags.protect_initialized = true; });
            DEVICE_STATE_I(PROTECT_LOG_TAG, "protect: lifecycle old=starting new=ready result=ok");
        }

        xTaskDelayUntil(&ticks, configTICK_RATE_HZ / protect_check_HZ);
    }
}

esp_err_t protect_init() {
    if (protect_task_handle) {
        ESP_LOGW(PROTECT_LOG_TAG, "protect task already running");
        return ESP_OK;
    }
    // 启动任务前先确定实际阈值，确保首次轮询和启动日志使用同一组配置。
    ensure_protect_config_loaded();
    log_initial_thresholds();
    reset_pending_protect_states();
    // 保护状态切换包含日志和订阅回调，按实测峰值额外保留约 1.7KB。
    constexpr uint32_t protect_task_stack_size = 3584;
    if (xTaskCreate(protect_task, "protect_task", protect_task_stack_size, nullptr, 5, &protect_task_handle) !=
        pdPASS) {
        protect_task_handle = nullptr;
        ESP_LOGE(PROTECT_LOG_TAG, "failed to create protect task");
        return ESP_ERR_NO_MEM;
    }
    protect_mos_fault_start();
    return ESP_OK;
}
bool protect_init_ok() {
    return _protect_init_ok;
}
esp_err_t protect_deinit() {
    protect_mos_fault_stop();
    if (protect_task_handle) {
        update_global_state([](GlobalState& state) {
            state.protect_states.protect_states_raw = 0; // 清除保护状态
            state.flags.protect_initialized         = false;
        });
        reset_pending_protect_states();
        vTaskDelete(protect_task_handle);
        protect_task_handle = nullptr;
        DEVICE_STATE_I(PROTECT_LOG_TAG, "protect: lifecycle old=ready new=stopped result=ok");
    } else {
        ESP_LOGW(PROTECT_LOG_TAG, "protect task not running");
    }
    return ESP_OK;
}

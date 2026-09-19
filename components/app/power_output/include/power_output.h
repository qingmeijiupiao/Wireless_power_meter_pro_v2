/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 功率输出控制模块，基于策略链架构实现可扩展的开关条件检查
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-04-30 12:55:16
 */
#ifndef POWER_OUTPUT_H
#define POWER_OUTPUT_H

#include "esp_err.h"
#include "driver/gpio.h"
#include <functional>
#include <cstdint>

namespace ShortCircuitDetect { struct Result; }

namespace PowerOutput {

/** 输出操作结果枚举 */
enum class OutputResult : uint8_t {
    OK = 0,               /**< 操作成功 */
    FAIL_NOT_INIT,        /**< 模块未初始化 */
    FAIL_PROTECT_ACTIVE,  /**< 保护状态激活，阻止开启 */
    FAIL_COOLDOWN_ACTIVE, /**< 冷却时间未到，阻止操作 */
    FAIL_SHORT_CIRCUIT,   /**< 开启前检测到短路 */
    FAIL_SHORT_DETECT,    /**< 检测或测试激励清理失败 */
    FAIL_BUSY,            /**< 已有检测/开启事务 */
    FAIL_CANCELLED,       /**< 关闭、保护或生命周期操作取消了请求 */
    FAIL_TIMEOUT,         /**< 等待超时，未提交的开启已取消 */
    FAIL_GPIO,            /**< 主输出 GPIO 操作失败 */
    PENDING,              /**< 异步请求已接受，尚未完成 */
};

/** 输出操作类型枚举 */
enum class OutputOperation : uint8_t {
    ON,  /**< 开启操作 */
    OFF, /**< 关闭操作 */
    TOGGLE, /**< 按当前输出/待开启状态切换 */
};

/**
 * @brief 输出策略基类，所有开关条件检查策略的接口
 * @note  继承此类实现自定义开启策略，通过 add_policy() 注册到策略链中，
 *        开启前依次调用 check()，全部通过才执行；关闭不受策略阻断。
 *        操作完成后调用 on_state_applied() 通知策略更新内部状态。
 *        策略在事务锁内执行，必须快速返回，不得重入输出服务。
 */
class OutputPolicy {
  public:
    /** @brief `~OutputPolicy` 接口。 */
    virtual ~OutputPolicy() = default;

    /**
     * @brief  检查当前操作是否允许执行
     * @param  op 当前请求的操作类型
     * @param  current_state 当前输出状态 (true=ON, false=OFF)
     * @return OK 允许执行，其他值阻止执行并返回原因
     */
    virtual OutputResult check(OutputOperation op, bool current_state) = 0;

    /**
     * @brief  操作已执行后的通知回调，用于策略更新内部状态
     * @param  op 已执行的操作类型
     * @param  new_state 执行后的输出状态
     */
    virtual void on_state_applied(OutputOperation op, bool new_state) = 0;
};

using OnOutputChangeCallback = std::function<void(bool new_state)>;
using CompletionCallback = std::function<void(OutputResult result, bool output_on)>;

/**
 * @brief 异步提交输出请求。source 必须是静态字符串。
 * @note 回调可能在本调用内或输出工作任务中执行，必须快速返回，不可调用同步开启/检测。
 *       每个请求恰好回调一次；PENDING 只表示接受，不能作为开启成功。
 *       检测中 OFF/TOGGLE 取消开启，重复 ON 返回 FAIL_BUSY。
 */
OutputResult request(OutputOperation op, const char* source, CompletionCallback completion = {});

/** 500ms 检测窗口另留 250ms 调度/清理余量；超时取消未提交请求。 */
constexpr uint32_t REQUEST_TIMEOUT_MS = 750;
const char* result_to_string(OutputResult result);

/** 非消费式运行快照，可由屏幕、通信等多个观察者独立读取。
 * request_id 标识最近一次有效事务；忙拒绝不覆盖正在执行的事务。
 * checking 仅表示事务未结束，output_on 始终是 GPIO 提交后的真实状态。
 * protect_mask 的 bit0..3 分别为 OTP/OVP/UVP/OCP，与持久化位域无关。
 */
struct Status {
    uint32_t request_id = 0;
    OutputResult result = OutputResult::OK;
    bool initialized = false;
    bool checking = false;
    bool output_on = false;
    bool bypassed = false;
    uint8_t protect_mask = 0;
    uint32_t cooldown_remaining_ms = 0;
};
/** 任务上下文调用，短时获取事务锁；不执行检测，不消费事件。 */
Status snapshot();

/** 手动检测也经过输出仲裁，输出开启或已有事务时拒绝；不自动开启输出。 */
OutputResult test_short_circuit(ShortCircuitDetect::Result& result, const char* source);

/** 单槽保留最近一次失败，供屏幕任务消费；重复失败合并，启动期间不会因 UI 未就绪丢失。 */
struct FailureNotice {
    OutputResult result = OutputResult::OK;
    uint16_t voltage_mV = 0;    /**< 短路失败时为窗口内最低有效采样电压。 */
    uint16_t threshold_mV = 0;
    esp_err_t error = ESP_OK;
};
bool take_failure_notice(FailureNotice& notice);

/** 单个轻量输出变化/失败唤醒回调，可在 init 前注册；事务锁外执行，不得阻塞。失败数据仍由 take_failure_notice 获取。 */
void set_event_notifier(void (*notifier)());

/**
 * @brief  初始化功率输出模块
 * @param  output_gpio 输出控制 GPIO 引脚号
 * @return ESP_OK 成功，其他值失败
 * @note   内部自动注册 ProtectPolicy 和 CooldownPolicy，
 *         并监听保护状态变更，保护触发时强制关闭输出
 */
esp_err_t init(gpio_num_t output_gpio);

/**
 * @brief  关闭输出并取消待开启；工作任务资源保留，用于完成脉冲清理及后续重新初始化
 * @return ESP_OK 成功
 */
esp_err_t deinit();

/**
 * @brief  同步开启输出，等待开启前检测及最终结果；source 必须为静态字符串
 * @return OK 表示已开启；失败原因见 OutputResult（保护、冷却、检测、取消、超时、GPIO 等），不会返回 PENDING。
 */
OutputResult on(const char* source);

/**
 * @brief  关闭输出
 * @return OK 成功，FAIL_NOT_INIT 未初始化，FAIL_GPIO 关闭 GPIO 失败
 */
OutputResult off(const char* source);

/**
 * @brief  切换输出状态（开->关，关->开）
 * @return 开启时等待最终结果，关闭立即返回；完整失败原因见 OutputResult，不会返回 PENDING。
 */
OutputResult toggle(const char* source);

/**
 * @brief  获取当前输出状态
 * @return true 输出开启，false 输出关闭
 */
bool get_state();

/**
 * @brief  注册输出状态变更回调
 * @param  cb 回调函数，参数为新的输出状态
 */
void add_on_change_callback(OnOutputChangeCallback cb);

/**
 * @brief  注册自定义策略到策略链末尾
 * @param  policy 策略对象指针（调用方需保证对象生命周期）
 * @note   策略按注册顺序依次检查，任一策略返回非 OK 即阻止操作
 */
void add_policy(OutputPolicy* policy);

} // namespace PowerOutput

#endif

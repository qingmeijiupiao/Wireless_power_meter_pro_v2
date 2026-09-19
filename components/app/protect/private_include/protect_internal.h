#ifndef PROTECT_INTERNAL_H
#define PROTECT_INTERNAL_H

#include <cstdint>

/**
 * @brief 判断 INA228 电压/电流测量链路当前是否可靠。
 *
 * @return true INA228 已初始化且没有 I2C 初始化错误、读取超时；false 测量处于降级或恢复中。
 *
 * @note OVP、UVP、OCP 和 MOS 诊断都依赖 INA228。测量不可靠时这些逻辑不得触发输出关断，
 *       只能记录降级状态，避免把通信异常误判为真实电气故障。
 */
bool protect_ina228_measurement_reliable();

/**
 * @brief 判断保护通道是否依赖 INA228 测量数据。
 *
 * @param channel 保护通道编号：0=OTP，1=OVP，2=UVP，3=OCP。
 * @return true 通道依赖 INA228；false 通道不依赖 INA228。
 *
 * @note 该接口用于选择触发迟滞时长和测量可靠性门控。OTP 使用温度传感器，不受 INA228 降级影响。
 */
bool protect_is_ina228_channel(uint32_t channel);

/**
 * @brief 启动 MOS 损坏诊断任务。
 *
 * @note 诊断任务独立于保护轮询任务，使用更低频率和更长确认时间，仅上报可疑硬件故障，
 *       不修改保护状态，也不直接改变输出状态。
 */
void protect_mos_fault_start();

/**
 * @brief 停止 MOS 损坏诊断任务。
 *
 * @note protect_deinit() 调用本接口，避免保护组件反初始化后访问全局测量状态。
 */
void protect_mos_fault_stop();

#endif

/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: CAN回调函数管理
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-01 18:38:43
 */
#ifndef CAN_CALLBACK_H
#define CAN_CALLBACK_H

#include "esp_err.h"
#include "HXC_TWAI.h"
#include "HXC_NVS.h"
#include "nvs_gpio_output.h"
#include "protect.h"

namespace CanCallback {

enum CALLBACK_ID : uint8_t {
    CALLBACK_PING         = 0x00,
    CALLBACK_GET_STATE    = 0x01,
    CALLBACK_SET_OUTPUT   = 0x02,
    CALLBACK_SET_RESISTOR = 0x03,
};

// 获取状态CAN消息数据结构
struct CALLBACK_GET_STATE_DATA_t {
    uint16_t voltage_mV;
    int16_t  current_mA;

    int8_t Board_temperature; // TMP235温度  单位为1摄氏度
    int8_t Chip_temperature;  // 芯片温度    单位为1摄氏度

    uint8_t output_state      : 1; // 输出状态     0: 关闭 1: 开启
    uint8_t current_direction : 1; // 当前电流方向 0: 正向 1: 反向
    uint8_t CAN_resistor      : 1; // 终端电阻状态 0: 关闭 1: 开启
    uint8_t reserved          : 5; // 保留位

    ProtectState_t UVP_flag : 2; // 低压保护状态
    ProtectState_t OVP_flag : 2; // 高压保护状态
    ProtectState_t OTP_flag : 2; // 过温保护状态
    ProtectState_t OCP_flag : 2; // 过流保护状态
} __attribute__((packed));
static_assert(sizeof(CALLBACK_GET_STATE_DATA_t) <= 8, "CAN_CALLBACK_GET_STATE_DATA_t size OVERFLOW");

// 设置输出CAN消息数据结构
struct CALLBACK_SET_OUTPUT_DATA_t {
    bool output_state;
} __attribute__((packed));

// 设置终端电阻CAN消息数据结构
struct CALLBACK_SET_RESISTOR_DATA_t {
    bool CAN_resistor_state;
} __attribute__((packed));

constexpr uint32_t DEFAULT_CAN_BAUDRATE = 1_Mbps;
constexpr uint32_t DEFAULT_DEVICE_CAN_ID = 0x400; // 默认设备ID 设备ID大于0x700时使用扩展帧，否则使用标准帧
extern HXC::NVS_DATA<uint32_t> CAN_BAUDRATE;
extern HXC::NVS_DATA<uint32_t> CAN_ID;

/**
 * @brief : CAN回调功能初始化
 * @return  {*}
 */
esp_err_t init();

/** @brief Return whether the CAN controller completed initialization. */
bool is_available();

/**
 * @brief : 获取CAN对象实例
 * @return  {*}
 */
HXC_TWAI& get_can_bus();

/**
 * @brief 获取 CAN 终端电阻控制器。
 * @return 控制器引用。
 */
NvsGpioOutput& terminal_resistor();

/** @return 终端电阻是否接入。 */
bool terminal_resistor_enabled();

/**
 * @brief 设置终端电阻状态并持久化。
 * @param enabled true 表示接入终端电阻。
 * @return ESP_OK 成功。
 */
esp_err_t set_terminal_resistor(bool enabled);

/**
 * @brief 翻转终端电阻状态并持久化。
 * @return ESP_OK 成功。
 */
esp_err_t toggle_terminal_resistor();

} // namespace CanCallback

#endif

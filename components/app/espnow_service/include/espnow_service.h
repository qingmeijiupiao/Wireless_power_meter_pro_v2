#ifndef ESPNOW_SERVICE_H
#define ESPNOW_SERVICE_H

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "espnow_link.h"
#include "espnow_service_proto.h"

namespace EspNowService {

/** @brief 注册接收端产品协议支持的全部 ESP-NOW 消息回调。 */
esp_err_t init();

/** @brief 返回本次运行中观察到的远程开关及其最近电量。 */
bool get_remote_switch_status(RemoteSwitchStatus& status);

/**
 * @brief 尽力向已配对设备发送本机电量，不等待链路 ACK
 * @param battery_percent 电量百分比，范围 0..100
 */
esp_err_t send_remote_battery(const EspNowLink::MacAddress& destination,
                              uint8_t battery_percent,
                              EspNowLink::SendCallback callback = nullptr,
                              void* context = nullptr);

/**
 * @brief 尽力发送周期数据，不等待业务响应
 *
 * 单播使用已配对 peer 加密，广播由 espnow_link 自动改为明文尽力传输。
 */
esp_err_t send_periodic_data(const EspNowLink::MacAddress& destination,
                             const DeviceData& data,
                             EspNowLink::SendCallback callback = nullptr,
                             void* context = nullptr);

} // namespace EspNowService

#endif

/*
 * @Description: ESP-NOW 产品业务消息注册、接收处理与主动上报
 *
 * 本文件直接承接 espnow_link 按 message_id 分发的消息。每个接收回调在一个函数内完成：
 * 传输语义检查 -> payload 长度/字段检查 -> 小端解码 -> 产品业务处理 -> 必要时发送响应。
 * 回调运行在 espnow_link 任务中，不允许阻塞、长时间等待或执行耗时外设操作。
 */
#include "espnow_service.h"

#include <cstdio>

#include "diagnostic_log.h"
#include "energy_meter.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "espnow_codec.h"
#include "espnow_service_proto.h"
#include "freertos/FreeRTOS.h"
#include "global_state.h"
#include "power_output.h"

namespace EspNowService {
namespace Internal {
namespace {

template <typename Handler> struct CallbackSlot {
    Handler handler;
    void*   context;
};

constexpr char TAG[] = "EspNowService";

portMUX_TYPE                        callback_lock         = portMUX_INITIALIZER_UNLOCKED;
CallbackSlot<SwitchResponseHandler> switch_response_slot  = {};
CallbackSlot<DataReceivedHandler>   data_received_slot    = {};
RemoteSwitchStatus                  remote_switch_status  = {};
EspNowLink::MacAddress              remote_switch_address = {};

/**
 * @brief 原子读取应用回调及其上下文
 *
 * setter 可能由其他任务调用。这里只在临界区内复制两个指针，真正执行用户回调时已经
 * 退出临界区，避免用户代码延长关中断时间。
 */
template <typename Handler> CallbackSlot<Handler> callback_snapshot(const CallbackSlot<Handler>& slot) {
    portENTER_CRITICAL(&callback_lock);
    const CallbackSlot<Handler> result = slot;
    portEXIT_CRITICAL(&callback_lock);
    return result;
}

/** @brief 原子更新应用回调；清除 handler 时同步清除失效的 context。 */
template <typename Handler> void set_callback(CallbackSlot<Handler>* slot, Handler handler, void* context) {
    portENTER_CRITICAL(&callback_lock);
    slot->handler = handler;
    slot->context = handler == nullptr ? nullptr : context;
    portEXIT_CRITICAL(&callback_lock);
}

// Stable wire reasons, independent of the product's OutputResult enum numbering.
uint8_t switch_reason(PowerOutput::OutputResult result) {
    using R = PowerOutput::OutputResult;
    switch (result) {
    case R::OK: return 0;
    case R::FAIL_SHORT_CIRCUIT: return 1;
    case R::FAIL_PROTECT_ACTIVE: return 2;
    case R::FAIL_COOLDOWN_ACTIVE: return 3;
    case R::FAIL_BUSY: return 4;
    case R::FAIL_CANCELLED: return 5;
    case R::FAIL_NOT_INIT: return 6;
    case R::FAIL_SHORT_DETECT: return 7;
    case R::FAIL_GPIO: return 8;
    case R::FAIL_TIMEOUT: return 9;
    default: return 10;
    }
}

uint8_t active_protection_mask() {
    if (protect_is_bypassed()) return 0;
    const auto s = get_global_state().protect_states.states_bit;
    return (s.temperature_protect_state == PROTECT_STATE_PROTECT ? 1 : 0) |
           (s.high_voltage_protect_state == PROTECT_STATE_PROTECT ? 2 : 0) |
           (s.low_voltage_protect_state == PROTECT_STATE_PROTECT ? 4 : 0) |
           (s.current_protect_state == PROTECT_STATE_PROTECT ? 8 : 0);
}

/** @brief 记录本次运行已经收到合法控制包，并锁定对应遥控器地址。 */
void mark_remote_switch_connected(const EspNowLink::MacAddress& source) {
    portENTER_CRITICAL(&callback_lock);
    if (!remote_switch_status.connected || remote_switch_address != source) {
        remote_switch_status  = {};
        remote_switch_address = source;
    }
    remote_switch_status.connected = true;
    portEXIT_CRITICAL(&callback_lock);
}

/**
 * @brief 处理远端输出控制请求并返回实际执行结果
 *
 * payload:
 * - [0..3] request_id，小端 uint32_t，0 为非法值
 * - [4]    SwitchAction
 *
 * 输出保护、初始化状态等产品约束由 PowerOutput 决定；无论执行成功与否都会返回当前
 * 实际输出状态，便于请求方校准自身显示。
 */
void on_switch_request(const EspNowLink::Message& message, void*) {
    // 先验证传输语义、固定长度和值域，再访问 payload 中的对应字段。
    if (!is_reliable_unicast(message) || message.payload_size != SWITCH_REQUEST_SIZE ||
        message.payload[4] > static_cast<uint8_t>(SwitchAction::TOGGLE)) {
        return;
    }
    const uint32_t request_id = EspNowLink::Codec::load_le<uint32_t>(message.payload);
    if (request_id == 0) {
        return;
    }
    const SwitchAction action = static_cast<SwitchAction>(message.payload[4]);
    mark_remote_switch_connected(message.source);

    // Copy request metadata: the receive payload is only valid inside this callback.
    const auto peer = message.source;
    const int64_t started_us = esp_timer_get_time();
    const auto operation = action == SwitchAction::OFF ? PowerOutput::OutputOperation::OFF :
                           action == SwitchAction::ON ? PowerOutput::OutputOperation::ON :
                                                        PowerOutput::OutputOperation::TOGGLE;
    PowerOutput::request(operation, TAG,
        [peer, request_id, action, started_us](PowerOutput::OutputResult output_result, bool output_on) {
            SwitchResponse response{};
            response.request_id = request_id;
            response.action = action;
            switch (output_result) {
            case PowerOutput::OutputResult::OK:
                response.result = SwitchResult::OK;
                break;
            case PowerOutput::OutputResult::FAIL_NOT_INIT:
                response.result = SwitchResult::NOT_READY;
                break;
            case PowerOutput::OutputResult::FAIL_SHORT_CIRCUIT:
            case PowerOutput::OutputResult::FAIL_BUSY:
            case PowerOutput::OutputResult::FAIL_CANCELLED:
            case PowerOutput::OutputResult::FAIL_PROTECT_ACTIVE:
            case PowerOutput::OutputResult::FAIL_COOLDOWN_ACTIVE:
                response.result = SwitchResult::REJECTED;
                break;
            default:
                response.result = SwitchResult::INTERNAL_ERROR;
                break;
            }
            response.output_on = output_on;

            // 业务处理耗时和来源 MAC 同时写日志与黑匣子，便于定位远程控制失败。
            char mac[18] = {};
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", peer.bytes[0], peer.bytes[1],
                     peer.bytes[2], peer.bytes[3], peer.bytes[4], peer.bytes[5]);
            const int64_t elapsed_us = esp_timer_get_time() - started_us;
            DEVICE_EVENT_I(TAG, "espnow: switch peer=%s action=%u result=%u output=%u process_us=%lld", mac,
                           static_cast<uint32_t>(action), static_cast<uint32_t>(response.result), response.output_on ? 1U : 0U,
                           static_cast<int64_t>(elapsed_us));

            // 响应复用请求 ID；可靠发送的链路 ACK 由 espnow_link 自动处理。
            uint8_t      payload[7] = {};
            const size_t size       = encode_switch_response(response, payload, sizeof(payload));
            EspNowLink::send(peer, MSG_SWITCH_RESPONSE, payload, size, reliable_options());
            // [request_id LE32, action, legacy result, output, reason, protection mask].
            // Old remotes ignore this message; new remotes can explain an ON refusal.
            uint8_t detail[9] = {};
            EspNowLink::Codec::store_le<uint32_t>(detail, request_id);
            detail[4] = static_cast<uint8_t>(action);
            detail[5] = static_cast<uint8_t>(response.result);
            detail[6] = output_on ? 1 : 0;
            detail[7] = switch_reason(output_result);
            detail[8] = active_protection_mask();
            EspNowLink::send(peer, MSG_SWITCH_DETAIL, detail, sizeof(detail), reliable_options());
        });
}

/** @brief 接收当前遥控器在控制包之后发送的尽力电量上报。 */
void on_remote_battery(const EspNowLink::Message& message, void*) {
    if (message.reliable || message.destination.is_broadcast() || message.payload_size != REMOTE_BATTERY_SIZE ||
        message.payload[0] > 100) {
        return;
    }

    bool accepted = false;
    portENTER_CRITICAL(&callback_lock);
    if (remote_switch_status.connected && remote_switch_address == message.source) {
        remote_switch_status.battery_percent = message.payload[0];
        remote_switch_status.battery_valid   = true;
        accepted                             = true;
    }
    portEXIT_CRITICAL(&callback_lock);

    if (accepted) {
        ESP_LOGI(TAG, "espnow: remote_battery peer=%02X:%02X:%02X:%02X:%02X:%02X percent=%u",
                       message.source.bytes[0], message.source.bytes[1], message.source.bytes[2],
                       message.source.bytes[3], message.source.bytes[4], message.source.bytes[5],
                       static_cast<uint32_t>(message.payload[0]));
    }
}

/**
 * @brief 接收开关控制响应并通知本机请求方
 *
 * meter 产品通常不主动发送开关请求，但保留该协议处理能力，使组件支持双向产品场景。
 * payload 在回调返回后失效，因此先解码为值类型，再调用应用通知。
 */
void on_switch_response(const EspNowLink::Message& message, void*) {
    if (!is_reliable_unicast(message) || message.payload_size != SWITCH_RESPONSE_SIZE ||
        message.payload[4] > static_cast<uint8_t>(SwitchAction::TOGGLE) ||
        message.payload[5] > static_cast<uint8_t>(SwitchResult::INTERNAL_ERROR) || message.payload[6] > 1) {
        return;
    }
    const uint32_t request_id = EspNowLink::Codec::load_le<uint32_t>(message.payload);
    if (request_id == 0) {
        return;
    }

    const auto slot = callback_snapshot(switch_response_slot);
    if (slot.handler != nullptr) {
        slot.handler(message.source, request_id, static_cast<SwitchAction>(message.payload[4]),
                     static_cast<SwitchResult>(message.payload[5]), message.payload[6] != 0, slot.context);
    }
}

/**
 * @brief 处理实时数据读取请求并返回当前产品快照
 *
 * 请求 payload 仅包含非零 request_id。响应数据来自 global_state、energy_meter 和
 * power_output，同一次请求内读取并编码为一个固定 40 字节快照。
 */
void on_data_request(const EspNowLink::Message& message, void*) {
    if (!is_reliable_unicast(message) || message.payload_size != DATA_REQUEST_SIZE) {
        return;
    }
    const uint32_t request_id = EspNowLink::Codec::load_le<uint32_t>(message.payload);
    if (request_id == 0) {
        return;
    }

    DataMessage response                    = {};
    response.request_id                     = request_id;
    const int64_t               started_us  = esp_timer_get_time();
    const auto                  state       = get_global_state();
    const EnergyMeter::Snapshot meter       = EnergyMeter::snapshot();
    response.available                      = true;
    response.data.voltage_mv                = state.voltage_mV;
    response.data.current_ua                = state.current_uA;
    response.data.board_temperature_centi_c = state.board_temperature;
    response.data.chip_temperature_centi_c  = state.chip_temperature;
    response.data.charge_uah                = meter.charge_uah;
    response.data.energy_uwh                = meter.energy_uwh;
    response.data.meter_time_ms             = meter.meter_time_ms;
    response.data.status_flags              = (PowerOutput::get_state() ? DEVICE_STATUS_OUTPUT_ON : 0) |
                                             (active_protection_mask() << 1);

    // 数据生成耗时用于识别业务回调是否拖慢 espnow_link 分发任务。
    char mac[18] = {};
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", message.source.bytes[0], message.source.bytes[1],
             message.source.bytes[2], message.source.bytes[3], message.source.bytes[4], message.source.bytes[5]);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    ESP_LOGI(TAG, "espnow: data_request peer=%s voltage_mv=%u current_ua=%ld process_us=%lld", mac,
                   response.data.voltage_mv, static_cast<int32_t>(response.data.current_ua),
                   static_cast<int64_t>(elapsed_us));
    // 即使未来数据暂不可用，也应发送 available=false 的同格式响应，而不是静默超时。
    uint8_t      payload[40] = {};
    const size_t size        = encode_data_message(response, payload, sizeof(payload));
    EspNowLink::send(message.source, MSG_DATA_RESPONSE, payload, size, reliable_options());
}

/**
 * @brief 解码可靠数据响应并通知应用
 *
 * request_id 必须非零，用于与本机之前发出的读取请求关联。available=false 时字段仍会
 * 完整解码，但调用方应以 available 为准决定是否采用数据。
 */
void on_data_response(const EspNowLink::Message& message, void*) {
    if (!is_reliable_unicast(message) || message.payload_size != DATA_MESSAGE_SIZE || message.payload[4] > 1) {
        return;
    }

    DataMessage data = {};
    data.request_id  = EspNowLink::Codec::load_le<uint32_t>(message.payload);
    if (data.request_id == 0) {
        return;
    }
    data.available                      = message.payload[4] != 0;
    data.data.status_flags              = message.payload[5];
    data.data.voltage_mv                = EspNowLink::Codec::load_le<uint16_t>(message.payload + 6);
    data.data.current_ua                = EspNowLink::Codec::load_le<int32_t>(message.payload + 8);
    data.data.board_temperature_centi_c = EspNowLink::Codec::load_le<int16_t>(message.payload + 12);
    data.data.chip_temperature_centi_c  = EspNowLink::Codec::load_le<int16_t>(message.payload + 14);
    data.data.charge_uah                = EspNowLink::Codec::load_le<int64_t>(message.payload + 16);
    data.data.energy_uwh                = EspNowLink::Codec::load_le<int64_t>(message.payload + 24);
    data.data.meter_time_ms             = EspNowLink::Codec::load_le<uint64_t>(message.payload + 32);

    const auto slot = callback_snapshot(data_received_slot);
    if (slot.handler != nullptr) {
        slot.handler(message.source, data.request_id, data.data, data.available, false, slot.context);
    }
}

/**
 * @brief 解码尽力传输的周期数据上报
 *
 * 周期上报固定使用 request_id=0、available=true、BEST_EFFORT。该约束可避免把请求响应
 * 误当成广播遥测，也避免周期包进入可靠重传流程。
 */
void on_periodic_data(const EspNowLink::Message& message, void*) {
    if (message.reliable || message.payload_size != DATA_MESSAGE_SIZE || message.payload[4] != 1 ||
        EspNowLink::Codec::load_le<uint32_t>(message.payload) != 0) {
        return;
    }

    DeviceData data                = {};
    data.status_flags              = message.payload[5];
    data.voltage_mv                = EspNowLink::Codec::load_le<uint16_t>(message.payload + 6);
    data.current_ua                = EspNowLink::Codec::load_le<int32_t>(message.payload + 8);
    data.board_temperature_centi_c = EspNowLink::Codec::load_le<int16_t>(message.payload + 12);
    data.chip_temperature_centi_c  = EspNowLink::Codec::load_le<int16_t>(message.payload + 14);
    data.charge_uah                = EspNowLink::Codec::load_le<int64_t>(message.payload + 16);
    data.energy_uwh                = EspNowLink::Codec::load_le<int64_t>(message.payload + 24);
    data.meter_time_ms             = EspNowLink::Codec::load_le<uint64_t>(message.payload + 32);

    const auto slot = callback_snapshot(data_received_slot);
    if (slot.handler != nullptr) {
        slot.handler(message.source, 0, data, true, true, slot.context);
    }
}

} // namespace
} // namespace Internal

esp_err_t init() {
    // service 负责完整启动链路，调用方无需重复初始化 EspNowLink。
    ESP_ERROR_CHECK(EspNowLink::init());

    // 每个业务 ID 直接绑定唯一处理函数，接收路径无需二次分发。
    ESP_ERROR_CHECK(EspNowLink::register_handler(Internal::MSG_SWITCH_REQUEST, Internal::on_switch_request));
    ESP_ERROR_CHECK(EspNowLink::register_handler(Internal::MSG_SWITCH_RESPONSE, Internal::on_switch_response));
    ESP_ERROR_CHECK(EspNowLink::register_handler(Internal::MSG_REMOTE_BATTERY, Internal::on_remote_battery));
    ESP_ERROR_CHECK(EspNowLink::register_handler(Internal::MSG_DATA_REQUEST, Internal::on_data_request));
    ESP_ERROR_CHECK(EspNowLink::register_handler(Internal::MSG_DATA_RESPONSE, Internal::on_data_response));
    ESP_ERROR_CHECK(EspNowLink::register_handler(Internal::MSG_DATA_PERIODIC, Internal::on_periodic_data));
    return ESP_OK;
}

bool get_remote_switch_status(RemoteSwitchStatus& status) {
    portENTER_CRITICAL(&Internal::callback_lock);
    status = Internal::remote_switch_status;
    portEXIT_CRITICAL(&Internal::callback_lock);
    return status.connected;
}

esp_err_t send_remote_battery(const EspNowLink::MacAddress& destination, uint8_t battery_percent,
                              EspNowLink::SendCallback callback, void* context) {
    uint8_t      payload[1] = {};
    const size_t size       = Internal::encode_remote_battery(battery_percent, payload, sizeof(payload));
    if (size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    EspNowLink::SendOptions options = {};
    options.delivery                = EspNowLink::Delivery::BEST_EFFORT;
    return EspNowLink::send(destination, Internal::MSG_REMOTE_BATTERY, payload, size, options, callback, context);
}

/**
 * @brief 编码并发送周期产品数据
 *
 * 周期上报没有业务 request_id，也不等待业务响应。目的地址为广播时，espnow_link 会
 * 强制使用明文 BEST_EFFORT；单播时仍使用已配置 peer。
 */
esp_err_t send_periodic_data(const EspNowLink::MacAddress& destination, const DeviceData& data,
                             EspNowLink::SendCallback callback, void* context) {
    Internal::DataMessage message       = {};
    message.available                   = true;
    message.data                        = data;
    uint8_t                 payload[40] = {};
    const size_t            size        = Internal::encode_data_message(message, payload, sizeof(payload));
    EspNowLink::SendOptions options     = {};
    options.delivery                    = EspNowLink::Delivery::BEST_EFFORT;
    return EspNowLink::send(destination, Internal::MSG_DATA_PERIODIC, payload, size, options, callback, context);
}

} // namespace EspNowService

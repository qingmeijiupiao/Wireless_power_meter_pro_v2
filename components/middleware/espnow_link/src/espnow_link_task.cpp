#include "espnow_link_internal.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "espnow_protocol.h"

namespace EspNowLink::Internal {
namespace {

static constexpr char TAG[] = "EspNowLink";

enum class DriverOwner : uint8_t {
    NONE = 0,
    PENDING,
    ACK,
};

DriverOwner driver_owner = DriverOwner::NONE;
AckRequest  active_ack   = {};

uint16_t timeout_for(const SendRequest& request) {
    if (request.options.ack_timeout_ms != 0) {
        return request.options.ack_timeout_ms;
    }
    PeerEntry* peer = find_peer(request.destination);
    if (request.options.timeout_mode == TimeoutMode::ADAPTIVE && peer != nullptr && peer->metrics.ack_timeout_ms != 0) {
        return peer->metrics.ack_timeout_ms;
    }
    return default_reliable_options.ack_timeout_ms;
}

void finish_pending(SendResult result) {
    if (pending.request.callback != nullptr) {
        pending.request.callback(result, pending.sequence, pending.request.context);
    }
    pending = {};
}

esp_err_t submit_pending(bool retry) {
    if (driver_owner != DriverOwner::NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    // IDF 建议等待前一次发送完成回调后再提交下一帧，因此全组件只保留一个驱动在途帧。
    esp_err_t ret = esp_now_send(pending.request.destination.bytes, pending.frame, pending.frame_size);
    if (ret != ESP_OK) {
        increment_counter(&statistics.tx_submit_errors);
        return ret;
    }
    increment_counter(&statistics.tx_packets);
    portENTER_CRITICAL(&statistics_lock);
    statistics.tx_bytes += static_cast<uint32_t>(pending.frame_size);
    portEXIT_CRITICAL(&statistics_lock);
    if (retry) {
        increment_counter(&statistics.tx_retries);
        pending.retransmitted = true;
    }
    pending.attempts++;
    if (pending.first_send_tick == 0) {
        pending.first_send_tick = xTaskGetTickCount();
    }
    pending.waiting_ack = false;
    driver_owner        = DriverOwner::PENDING;
    return ESP_OK;
}

void start_send(const SendRequest& request) {
    if (pending.active || !active) {
        if (request.callback != nullptr) {
            request.callback(SendResult::SUBMIT_FAILED, 0, request.context);
        }
        increment_counter(&statistics.tx_submit_errors);
        return;
    }

    pending          = {};
    pending.active   = true;
    pending.request  = request;
    pending.sequence = next_sequence++;
    if (next_sequence == 0) {
        next_sequence = 1;
    }
    const uint8_t flags = request.options.delivery == Delivery::RELIABLE ? FRAME_FLAG_RELIABLE : 0;
    if (encode_frame(flags, request.message_id, pending.sequence, local_session_id, request.payload,
                     request.payload_size, pending.frame, sizeof(pending.frame), &pending.frame_size) != ESP_OK) {
        finish_pending(SendResult::SUBMIT_FAILED);
        return;
    }

    if (request.options.delivery == Delivery::RELIABLE) {
        increment_counter(&statistics.tx_reliable_packets);
    } else {
        increment_counter(&statistics.tx_best_effort_packets);
    }
    if (submit_pending(false) != ESP_OK) {
        finish_pending(SendResult::SUBMIT_FAILED);
    }
}

void queue_ack(const MacAddress& destination, uint32_t session, uint32_t sequence) {
    AckRequest request  = {};
    request.destination = destination;
    request.session     = session;
    request.sequence    = sequence;
    if (xQueueSend(ack_queue, &request, 0) != pdTRUE) {
        increment_counter(&statistics.tx_submit_errors);
    }
}

void submit_next_ack() {
    if (driver_owner != DriverOwner::NONE || xQueueReceive(ack_queue, &active_ack, 0) != pdTRUE) {
        return;
    }
    // ACK 与业务包共用底层发送完成序列，通过 driver_owner 区分完成事件归属。
    uint8_t frame[FRAME_HEADER_SIZE] = {};
    size_t  frame_size               = 0;
    if (encode_frame(FRAME_FLAG_ACK, ACK_MESSAGE_ID, active_ack.session, active_ack.sequence, nullptr, 0, frame,
                     sizeof(frame), &frame_size) != ESP_OK) {
        return;
    }
    if (esp_now_send(active_ack.destination.bytes, frame, frame_size) == ESP_OK) {
        increment_counter(&statistics.tx_packets);
        portENTER_CRITICAL(&statistics_lock);
        statistics.tx_bytes += static_cast<uint32_t>(frame_size);
        portEXIT_CRITICAL(&statistics_lock);
        increment_counter(&statistics.ack_sent);
        driver_owner = DriverOwner::ACK;
    } else {
        increment_counter(&statistics.tx_submit_errors);
    }
}

void update_rtt(PeerEntry* peer) {
    if (peer == nullptr || pending.retransmitted) {
        return;
    }
    uint32_t rtt = static_cast<uint32_t>((xTaskGetTickCount() - pending.first_send_tick) * portTICK_PERIOD_MS);
    rtt          = std::max<uint32_t>(1, rtt);
    peer->metrics.last_rtt_ms                                  = static_cast<uint16_t>(rtt);
    peer->metrics.recent_rtt_ms[peer->metrics.recent_rtt_next] = static_cast<uint16_t>(rtt);
    peer->metrics.recent_rtt_next = static_cast<uint8_t>((peer->metrics.recent_rtt_next + 1U) % 3U);
    if (peer->metrics.recent_rtt_count < 3) {
        peer->metrics.recent_rtt_count++;
    }
    if (peer->metrics.rtt_sample_count < UINT8_MAX) {
        peer->metrics.rtt_sample_count++;
    }
    if (peer->metrics.smoothed_rtt_ms == 0) {
        peer->metrics.smoothed_rtt_ms = static_cast<uint16_t>(rtt);
    } else {
        peer->metrics.smoothed_rtt_ms = static_cast<uint16_t>((7U * peer->metrics.smoothed_rtt_ms + rtt) / 8U);
    }
    peer->metrics.ack_timeout_ms = static_cast<uint16_t>(
        std::clamp<uint32_t>(peer->metrics.smoothed_rtt_ms + 5U, MIN_ACK_TIMEOUT_MS, MAX_ACK_TIMEOUT_MS));
}

void process_ack(const RxEvent& event, const ParsedFrame& frame) {
    if (!pending.active) {
        // 事务已结束后才到达的迟到/重复 ACK（重传时对端可能对同一序号重复回 ACK）。
        // 这类包不代表时序异常，单独计入 late_acks，避免污染 timing_errors。
        increment_counter(&statistics.late_acks);
        return;
    }
    if (pending.request.options.delivery != Delivery::RELIABLE ||
        pending.request.destination != event.source || pending.sequence != frame.correlation ||
        local_session_id != frame.sequence) {
        increment_counter(&statistics.unexpected_acks);
        increment_counter(&statistics.timing_errors);
        return;
    }
    if (!pending.waiting_ack && driver_owner != DriverOwner::PENDING) {
        increment_counter(&statistics.late_acks);
        increment_counter(&statistics.timing_errors);
        return;
    }
    increment_counter(&statistics.ack_received);
    update_rtt(find_peer(event.source));
    finish_pending(SendResult::ACKNOWLEDGED);
}

} // namespace

void process_received_event(const RxEvent& event) {
    ParsedFrame frame = {};
    // 普通任务中执行 CRC32 和 ACK 字段语义等完整校验。
    if (!decode_frame(event.data, event.size, &frame)) {
        increment_counter(&statistics.rx_invalid_packets);
        return;
    }
    increment_counter(&statistics.rx_packets);
    __atomic_fetch_add(&statistics.rx_bytes, event.size, __ATOMIC_RELAXED);

    if ((frame.flags & FRAME_FLAG_ACK) != 0) {
        process_ack(event, frame);
        return;
    }

    PeerEntry* peer     = find_peer(event.source);
    const bool reliable = (frame.flags & FRAME_FLAG_RELIABLE) != 0;
    if (reliable) {
        if (event.destination.is_broadcast() || peer == nullptr) {
            increment_counter(&statistics.rx_invalid_packets);
            return;
        }
        // 同一 session 内仅接受递增序号；重复包只补发 ACK，不重复执行业务。
        if (peer->has_rx_sequence && peer->last_rx_session == frame.correlation) {
            if (frame.sequence == peer->last_rx_sequence) {
                increment_counter(&statistics.rx_duplicates);
                queue_ack(event.source, frame.correlation, frame.sequence);
                return;
            }
            if (frame.sequence < peer->last_rx_sequence) {
                increment_counter(&statistics.sequence_errors);
                increment_counter(&statistics.timing_errors);
                return;
            }
        }
        peer->last_rx_session  = frame.correlation;
        peer->last_rx_sequence = frame.sequence;
        peer->has_rx_sequence  = true;
        queue_ack(event.source, frame.correlation, frame.sequence);
    }

    Message message      = {};
    message.source       = event.source;
    message.destination  = event.destination;
    message.message_id   = frame.message_id;
    message.sequence     = frame.sequence;
    message.payload      = frame.payload;
    message.payload_size = frame.payload_size;
    message.rssi         = event.rssi;
    message.channel      = event.channel;
    message.reliable     = reliable;

    MessageHandler selected_handler = nullptr;
    void*          selected_context = nullptr;
    portENTER_CRITICAL(&state_lock);
    for (const auto& handler : handlers) {
        if (handler.used && handler.message_id == frame.message_id) {
            selected_handler = handler.handler;
            selected_context = handler.context;
            break;
        }
    }
    portEXIT_CRITICAL(&state_lock);
    if (selected_handler != nullptr) {
        selected_handler(message, selected_context);
    }
}

void process_mac_result(const MacResultEvent& event) {
    if (driver_owner == DriverOwner::NONE) {
        return;
    }
    if (driver_owner == DriverOwner::ACK) {
        if (!event.success) {
            increment_counter(&statistics.tx_mac_failures);
        }
        driver_owner = DriverOwner::NONE;
        return;
    }
    driver_owner = DriverOwner::NONE;
    if (!pending.active || pending.request.destination != event.destination) {
        return;
    }

    if (!event.success) {
        increment_counter(&statistics.tx_mac_failures);
        if (pending.request.options.delivery == Delivery::BEST_EFFORT) {
            finish_pending(SendResult::MAC_FAILED);
            return;
        }
    } else if (pending.request.options.delivery == Delivery::BEST_EFFORT) {
        finish_pending(SendResult::SENT);
        return;
    }

    pending.waiting_ack   = true;
    pending.deadline_tick = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_for(pending.request));
}

void process_timeout() {
    if (!pending.active || !pending.waiting_ack || driver_owner != DriverOwner::NONE) {
        return;
    }
    const TickType_t now = xTaskGetTickCount();
    if (static_cast<int32_t>(now - pending.deadline_tick) < 0) {
        return;
    }
    pending.waiting_ack = false;
    // 使用 APP/peer/全局三级配置得到的固定超时，不采用指数退避。
    if (pending.attempts >= pending.request.options.max_attempts) {
        increment_counter(&statistics.ack_timeouts);
        finish_pending(SendResult::NO_ACK);
        return;
    }
    if (submit_pending(true) != ESP_OK) {
        finish_pending(SendResult::SUBMIT_FAILED);
    }
}

void link_task(void*) {
    while (true) {
        // 优先清空接收与发送完成事件，降低 ACK 和控制包处理延时。
        RxEvent event = {};
        while (xQueueReceive(rx_queue, &event, 0) == pdTRUE) {
            process_received_event(event);
        }

        MacResultEvent mac_result = {};
        while (xQueueReceive(mac_queue, &mac_result, 0) == pdTRUE) {
            process_mac_result(mac_result);
        }

        if (driver_owner == DriverOwner::NONE) {
            submit_next_ack();
        }
        if (!pending.active && driver_owner == DriverOwner::NONE) {
            SendRequest request = {};
            if (xQueueReceive(tx_queue, &request, 0) == pdTRUE) {
                start_send(request);
            }
        }
        process_timeout();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

} // namespace EspNowLink::Internal

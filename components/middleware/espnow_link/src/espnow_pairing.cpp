#include "espnow_link.h"

#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "diagnostic_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "espnow_pairing_internal.h"
#include "espnow_protocol.h"
#include "wifi_manager.h"

namespace EspNowLink {
namespace Internal {
namespace {

static constexpr char        TAG[]                 = "EspNowPairing";
static constexpr size_t      EVENT_QUEUE_LENGTH    = 12;
static constexpr uint32_t    TASK_STACK_SIZE       = 4096;
static constexpr UBaseType_t TASK_PRIORITY         = 3;
static constexpr uint32_t    CHANNEL_WAIT_MS       = 40;
static constexpr uint32_t    PAIR_RESPONSE_WAIT_MS = 200;

bool                   initialized                       = false;
bool                   pairing_mode                      = false;
bool                   initiating_pairing                = false;
bool                   channel_recovering                = false;
esp_err_t              channel_recovery_result           = ESP_ERR_INVALID_STATE;
bool                   pair_transaction_active           = false;
QueueHandle_t          event_queue                       = nullptr;
TaskHandle_t           task_handle                       = nullptr;
TickType_t             pairing_deadline                  = 0;
bool                   pairing_has_deadline              = false;
uint32_t               active_nonce                      = 0;
EspNowLink::MacAddress pending_peer                      = {};
uint8_t                pending_lmk[EspNowLink::KEY_SIZE] = {};
uint8_t                pending_channel                   = 1;
uint8_t                initiator_restore_channel         = 1;

EspNowLink::MacAddress local_mac() {
    EspNowLink::MacAddress result = {};
    const MAC_t            mac    = WiFiManager::instance().get_mac(WIFI_IF_STA);
    memcpy(result.bytes, mac.bytes, sizeof(result.bytes));
    return result;
}

void enqueue_event(const PairEvent& event) {
    if (event_queue != nullptr && xQueueSend(event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "pairing event queue full");
    }
}

void link_message_handler(const EspNowLink::Message& message, void*) {
    // Link handler 只解析固定 payload 并入队，配对状态和 NVS 操作由服务任务处理。
    PairEvent event = {};
    event.source    = message.source;
    event.channel   = message.channel;

    switch (message.message_id) {
    case MSG_DISCOVERY_PING:
        event.type = PairEventType::DISCOVERY_PING;
        if (!decode_nonce(message, &event.nonce)) {
            return;
        }
        break;
    case MSG_DISCOVERY_RESPONSE:
        event.type = PairEventType::DISCOVERY_RESPONSE;
        if (!decode_discovery_response(message, local_mac(), &event.nonce)) {
            return;
        }
        break;
    case MSG_PAIR_REQUEST:
        event.type = PairEventType::PAIR_REQUEST;
        if (!decode_nonce(message, &event.nonce)) {
            return;
        }
        break;
    case MSG_PAIR_RESPONSE:
        event.type = PairEventType::PAIR_RESPONSE;
        if (!decode_pair_response(message, &event.nonce, event.lmk)) {
            return;
        }
        break;
    case MSG_PAIR_CONFIRM:
        event.type = PairEventType::PAIR_CONFIRM;
        if (!decode_nonce(message, &event.nonce)) {
            return;
        }
        break;
    case MSG_CHANNEL_PROBE:
        event.type = PairEventType::CHANNEL_PROBE;
        if (!decode_nonce(message, &event.nonce)) {
            return;
        }
        break;
    case MSG_CHANNEL_PROBE_RESPONSE:
        event.type = PairEventType::CHANNEL_PROBE_RESPONSE;
        if (!decode_nonce(message, &event.nonce)) {
            return;
        }
        break;
    default:
        return;
    }
    enqueue_event(event);
}

void pair_response_send_callback(EspNowLink::SendResult result, uint32_t, void*) {
    PairEvent event   = {};
    event.type        = PairEventType::PAIR_RESPONSE_SENT;
    event.source      = pending_peer;
    event.nonce       = active_nonce;
    event.send_result = result;
    enqueue_event(event);
}

void confirm_send_callback(EspNowLink::SendResult result, uint32_t, void*) {
    PairEvent event   = {};
    event.type        = PairEventType::CONFIRM_RESULT;
    event.source      = pending_peer;
    event.nonce       = active_nonce;
    event.send_result = result;
    enqueue_event(event);
}

void register_responder_handlers() {
    EspNowLink::register_handler(MSG_DISCOVERY_PING, link_message_handler);
    EspNowLink::register_handler(MSG_PAIR_REQUEST, link_message_handler);
    EspNowLink::register_handler(MSG_PAIR_CONFIRM, link_message_handler);
}

void unregister_responder_handlers() {
    EspNowLink::unregister_handler(MSG_DISCOVERY_PING);
    EspNowLink::unregister_handler(MSG_PAIR_REQUEST);
    EspNowLink::unregister_handler(MSG_PAIR_CONFIRM);
}

void register_initiator_handlers() {
    EspNowLink::register_handler(MSG_DISCOVERY_RESPONSE, link_message_handler);
    EspNowLink::register_handler(MSG_PAIR_RESPONSE, link_message_handler);
}

void unregister_initiator_handlers() {
    EspNowLink::unregister_handler(MSG_DISCOVERY_RESPONSE);
    EspNowLink::unregister_handler(MSG_PAIR_RESPONSE);
}

void send_discovery_response(const PairEvent& event) {
    uint8_t                 payload[EspNowLink::MAC_ADDRESS_SIZE + 4] = {};
    const size_t            size    = encode_discovery_response(event.source, event.nonce, payload, sizeof(payload));
    EspNowLink::SendOptions options = {};
    options.delivery                = EspNowLink::Delivery::BEST_EFFORT;
    options.allow_plaintext         = true;
    EspNowLink::send(EspNowLink::BROADCAST_ADDRESS, MSG_DISCOVERY_RESPONSE, payload, size, options);
}

void handle_pair_request(const PairEvent& event) {
    if (!pairing_mode || pair_transaction_active) {
        return;
    }
    if (saved_peer_count() >= MAX_SAVED_PEERS) {
        bool already_saved = false;
        for (size_t i = 0; i < saved_peer_count(); ++i) {
            SavedPeer saved = {};
            if (read_saved_peer(i, &saved) == ESP_OK && saved.address == event.source) {
                already_saved = true;
                break;
            }
        }
        if (!already_saved) {
            ESP_LOGW(TAG, "pairing rejected: peer table full");
            return;
        }
    }

    pair_transaction_active = true;
    // 单次响应窗口只处理一个候选设备，避免并发请求交叉覆盖临时 LMK。
    active_nonce            = event.nonce;
    pending_peer            = event.source;
    pending_channel         = event.channel;
    esp_fill_random(pending_lmk, sizeof(pending_lmk));

    EspNowLink::PeerConfig temporary_peer = {};
    temporary_peer.address                = event.source;
    temporary_peer.channel                = event.channel;
    temporary_peer.encrypted              = false;
    if (EspNowLink::add_peer(temporary_peer) != ESP_OK) {
        pair_transaction_active = false;
        return;
    }

    uint8_t                 payload[4 + EspNowLink::KEY_SIZE] = {};
    const size_t            size    = encode_pair_response(active_nonce, pending_lmk, payload, sizeof(payload));
    EspNowLink::SendOptions options = {};
    options.delivery                = EspNowLink::Delivery::BEST_EFFORT;
    options.allow_plaintext         = true;
    EspNowLink::send(event.source, MSG_PAIR_RESPONSE, payload, size, options, pair_response_send_callback);
}

void handle_pair_response_sent(const PairEvent& event) {
    if (event.send_result != EspNowLink::SendResult::SENT || event.source != pending_peer ||
        event.nonce != active_nonce) {
        EspNowLink::remove_peer(event.source);
        restore_peers();
        pair_transaction_active = false;
        return;
    }
    // 明文响应完成后立即把临时 peer 切换为加密 peer，后续确认必须加密。
    EspNowLink::PeerConfig peer = {};
    peer.address                = pending_peer;
    memcpy(peer.lmk, pending_lmk, sizeof(peer.lmk));
    peer.channel   = pending_channel;
    peer.encrypted = true;
    EspNowLink::add_peer(peer);
}

void handle_pair_confirm(const PairEvent& event) {
    if (!pairing_mode || event.source != pending_peer || event.nonce != active_nonce) {
        return;
    }
    EspNowLink::PeerConfig peer = {};
    peer.address                = pending_peer;
    memcpy(peer.lmk, pending_lmk, sizeof(peer.lmk));
    peer.channel   = pending_channel;
    peer.encrypted = true;
    if (save_peer(peer, pending_channel) == ESP_OK) {
        DEVICE_STATE_I(TAG, "espnow: peer action=paired role=responder channel=%u result=ok",
                       static_cast<uint32_t>(pending_channel));
        pair_transaction_active = false;
        leave_pairing_mode();
    }
}

bool wait_for_event(PairEventType type, uint32_t nonce, PairEvent* output, uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    PairEvent        event    = {};
    while (static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) {
        const TickType_t remaining = deadline - xTaskGetTickCount();
        if (xQueueReceive(event_queue, &event, remaining) != pdTRUE) {
            break;
        }
        // nonce 隔离不同信道和不同轮次的迟到响应。
        if (event.type == type && event.nonce == nonce) {
            if (output != nullptr) {
                *output = event;
            }
            return true;
        }
    }
    return false;
}

void send_channel_probe_response(const PairEvent& event) {
    uint8_t      payload[4]   = {};
    const size_t payload_size = encode_nonce(event.nonce, payload, sizeof(payload));
    SendOptions  options      = {};
    options.delivery          = Delivery::BEST_EFFORT;
    EspNowLink::send(event.source, MSG_CHANNEL_PROBE_RESPONSE, payload, payload_size, options);
}

bool probe_peer_on_channel(const MacAddress& peer, uint8_t channel, uint16_t wait_ms) {
    const uint32_t random_nonce                               = esp_random();
    const uint32_t nonce                                      = random_nonce == 0 ? 1 : random_nonce;
    uint8_t        payload[4]                                 = {};
    const size_t   payload_size                               = encode_nonce(nonce, payload, sizeof(payload));
    uint8_t        frame[FRAME_HEADER_SIZE + sizeof(payload)] = {};
    size_t         frame_size                                 = 0;
    uint32_t       sequence                                   = esp_random();
    if (sequence == 0) {
        sequence = 1;
    }
    uint32_t correlation = esp_random();
    if (correlation == 0) {
        correlation = 1;
    }
    if (encode_frame(0, MSG_CHANNEL_PROBE, sequence, correlation, payload, payload_size, frame, sizeof(frame),
                     &frame_size) != ESP_OK) {
        return false;
    }

    alignas(esp_now_switch_channel_t) uint8_t request_buffer[sizeof(esp_now_switch_channel_t) + sizeof(frame)] = {};
    auto*                                     request = reinterpret_cast<esp_now_switch_channel_t*>(request_buffer);
    request->type                                     = WIFI_OFFCHAN_TX_REQ;
    request->channel                                  = channel;
    request->sec_channel                              = WIFI_SECOND_CHAN_NONE;
    request->wait_time_ms                             = wait_ms;
    request->op_id                                    = static_cast<uint8_t>(sequence);
    memcpy(request->dest_mac, peer.bytes, sizeof(request->dest_mac));
    request->data_len = static_cast<uint16_t>(frame_size);
    memcpy(request->data, frame, frame_size);
    if (esp_now_switch_channel_tx(request) != ESP_OK) {
        return false;
    }

    PairEvent result = {};
    return wait_for_event(PairEventType::CHANNEL_PROBE_RESPONSE, nonce, &result, wait_ms) && result.source == peer;
}

// 单次 BEST_EFFORT 探测在 WiFi 共存环境下容易丢响应。首选信道多试几次可以避免
// 一次丢包就误判为“信道已变化”并升级为全信道扫描；扫描时每信道也重试一次。
constexpr uint8_t PREFERRED_PROBE_ATTEMPTS = 3;
constexpr uint8_t SCAN_PROBE_ATTEMPTS      = 2;

bool probe_peer_on_channel_retry(const MacAddress& peer, uint8_t channel, uint16_t wait_ms, uint8_t attempts) {
    for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
        if (probe_peer_on_channel(peer, channel, wait_ms)) {
            return true;
        }
    }
    return false;
}

void run_channel_recovery(const MacAddress& peer) {
    const uint16_t probe_timeout_ms  = get_channel_probe_timeout_ms(peer);
    SavedPeer      saved             = {};
    uint8_t        preferred_channel = 0;
    for (size_t i = 0; i < saved_peer_count(); ++i) {
        if (read_saved_peer(i, &saved) == ESP_OK && saved.address == peer) {
            preferred_channel = saved.last_channel;
            break;
        }
    }
    if (preferred_channel != 0 &&
        probe_peer_on_channel_retry(peer, preferred_channel, probe_timeout_ms, PREFERRED_PROBE_ATTEMPTS)) {
        WiFiManager::instance().set_channel(preferred_channel);
        update_peer_channel(peer, preferred_channel);
        channel_recovery_result = ESP_OK;
        channel_recovering      = false;
        ESP_LOGI(TAG, "peer channel unchanged: %u", preferred_channel);
        return;
    }

    wifi_country_t country = {};
    WiFiManager::instance().get_country(&country);
    const uint8_t first = country.schan == 0 ? 1 : country.schan;
    const uint8_t count = country.nchan == 0 ? 13 : country.nchan;
    for (uint8_t channel = first; channel < static_cast<uint8_t>(first + count); ++channel) {
        if (probe_peer_on_channel_retry(peer, channel, probe_timeout_ms, SCAN_PROBE_ATTEMPTS)) {
            WiFiManager::instance().set_channel(channel);
            update_peer_channel(peer, channel);
            channel_recovery_result = ESP_OK;
            channel_recovering      = false;
            DEVICE_STATE_I(TAG, "espnow: peer_channel state=recovered channel=%u result=ok",
                           static_cast<uint32_t>(channel));
            return;
        }
    }

    if (preferred_channel != 0) {
        WiFiManager::instance().set_channel(preferred_channel);
    }
    restore_peers();
    channel_recovery_result = ESP_ERR_TIMEOUT;
    channel_recovering      = false;
    ESP_LOGW(TAG, "peer channel recovery failed");
}

bool try_pair_on_channel(uint8_t channel) {
    // 主动配对要求当前设备可以切换 STA 射频信道完成发现扫描。
    if (WiFiManager::instance().set_channel(channel) != ESP_OK) {
        return false;
    }

    // 每个信道使用独立 nonce，避免上一信道的迟到广播被当前扫描轮次接受。
    active_nonce = esp_random();
    if (active_nonce == 0) {
        active_nonce = 1;
    }
    uint8_t                 nonce_payload[4] = {};
    const size_t            nonce_size       = encode_nonce(active_nonce, nonce_payload, sizeof(nonce_payload));
    EspNowLink::SendOptions broadcast        = {};
    broadcast.delivery                       = EspNowLink::Delivery::BEST_EFFORT;
    broadcast.allow_plaintext                = true;
    if (EspNowLink::send(EspNowLink::BROADCAST_ADDRESS, MSG_DISCOVERY_PING, nonce_payload, nonce_size, broadcast) !=
        ESP_OK) {
        return false;
    }

    PairEvent discovery = {};
    if (!wait_for_event(PairEventType::DISCOVERY_RESPONSE, active_nonce, &discovery, CHANNEL_WAIT_MS)) {
        return false;
    }

    pending_peer                          = discovery.source;
    pending_channel                       = channel;
    EspNowLink::PeerConfig temporary_peer = {};
    temporary_peer.address                = pending_peer;
    temporary_peer.channel                = channel;
    temporary_peer.encrypted              = false;
    if (EspNowLink::add_peer(temporary_peer) != ESP_OK) {
        return false;
    }

    EspNowLink::SendOptions request_options = {};
    request_options.delivery                = EspNowLink::Delivery::BEST_EFFORT;
    request_options.allow_plaintext         = true;
    if (EspNowLink::send(pending_peer, MSG_PAIR_REQUEST, nonce_payload, nonce_size, request_options) != ESP_OK) {
        EspNowLink::remove_peer(pending_peer);
        return false;
    }

    PairEvent response = {};
    if (!wait_for_event(PairEventType::PAIR_RESPONSE, active_nonce, &response, PAIR_RESPONSE_WAIT_MS)) {
        EspNowLink::remove_peer(pending_peer);
        return false;
    }

    memcpy(pending_lmk, response.lmk, sizeof(pending_lmk));
    EspNowLink::PeerConfig peer = {};
    peer.address                = pending_peer;
    memcpy(peer.lmk, pending_lmk, sizeof(peer.lmk));
    peer.channel   = channel;
    peer.encrypted = true;
    if (EspNowLink::add_peer(peer) != ESP_OK) {
        return false;
    }

    EspNowLink::SendOptions confirm_options = {};
    confirm_options.delivery                = EspNowLink::Delivery::RELIABLE;
    confirm_options.timeout_mode            = EspNowLink::TimeoutMode::FIXED;
    confirm_options.ack_timeout_ms          = 20;
    confirm_options.max_attempts            = 5;
    return EspNowLink::send(pending_peer, MSG_PAIR_CONFIRM, nonce_payload, nonce_size, confirm_options,
                            confirm_send_callback) == ESP_OK;
}

void run_initiator_pairing() {
    register_initiator_handlers();
    initiating_pairing = true;

    uint8_t   preferred_channel = 0;
    SavedPeer saved             = {};
    if (read_saved_peer(0, &saved) == ESP_OK) {
        preferred_channel = saved.last_channel;
    }
    initiator_restore_channel = preferred_channel;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (initiator_restore_channel == 0 &&
        WiFiManager::instance().get_channel(&initiator_restore_channel, &second) != ESP_OK) {
        initiator_restore_channel = 1;
    }
    // 先尝试上次成功信道，失败后再遍历国家码允许的完整信道范围。
    if (preferred_channel != 0 && try_pair_on_channel(preferred_channel)) {
        return;
    }

    wifi_country_t country = {};
    WiFiManager::instance().get_country(&country);
    const uint8_t first = country.schan == 0 ? 1 : country.schan;
    const uint8_t count = country.nchan == 0 ? 13 : country.nchan;
    for (uint8_t channel = first; channel < static_cast<uint8_t>(first + count) && initiating_pairing; ++channel) {
        if (channel == preferred_channel) {
            continue;
        }
        if (try_pair_on_channel(channel)) {
            return;
        }
    }

    initiating_pairing = false;
    unregister_initiator_handlers();
    WiFiManager::instance().set_channel(initiator_restore_channel);
    restore_peers();
    ESP_LOGW(TAG, "no pairable peer found");
}

void pairing_task(void*) {
    PairEvent event = {};
    while (true) {
        if (pairing_mode && pairing_has_deadline && static_cast<int32_t>(xTaskGetTickCount() - pairing_deadline) >= 0) {
            leave_pairing_mode();
        }

        if (xQueueReceive(event_queue, &event, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }
        // 所有配对状态转移在单一任务中串行完成，Link 回调不直接写 NVS。
        switch (event.type) {
        case PairEventType::START_PAIRING:
            run_initiator_pairing();
            break;
        case PairEventType::START_CHANNEL_RECOVERY:
            run_channel_recovery(event.source);
            break;
        case PairEventType::CHANNEL_PROBE:
            send_channel_probe_response(event);
            break;
        case PairEventType::DISCOVERY_PING:
            if (pairing_mode) {
                send_discovery_response(event);
            }
            break;
        case PairEventType::PAIR_REQUEST:
            handle_pair_request(event);
            break;
        case PairEventType::PAIR_RESPONSE_SENT:
            handle_pair_response_sent(event);
            break;
        case PairEventType::PAIR_CONFIRM:
            handle_pair_confirm(event);
            break;
        case PairEventType::CONFIRM_RESULT:
            if (event.send_result == EspNowLink::SendResult::ACKNOWLEDGED) {
                EspNowLink::PeerConfig peer = {};
                peer.address                = pending_peer;
                memcpy(peer.lmk, pending_lmk, sizeof(peer.lmk));
                peer.channel   = pending_channel;
                peer.encrypted = true;
                save_peer(peer, pending_channel);
                initiating_pairing = false;
                unregister_initiator_handlers();
                DEVICE_STATE_I(TAG, "espnow: peer action=paired role=initiator channel=%u result=ok",
                               static_cast<uint32_t>(pending_channel));
            } else {
                EspNowLink::remove_peer(pending_peer);
                initiating_pairing = false;
                unregister_initiator_handlers();
                WiFiManager::instance().set_channel(initiator_restore_channel);
                restore_peers();
                ESP_LOGW(TAG, "pair confirm failed");
            }
            break;
        default:
            break;
        }
    }
}

} // namespace
} // namespace Internal

esp_err_t Internal::init_pairing() {
    using namespace Internal;
    if (initialized) {
        return ESP_OK;
    }
    if (!EspNowLink::is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    event_queue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(PairEvent));
    if (event_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    restore_peers();
    esp_err_t ret = EspNowLink::register_handler(MSG_CHANNEL_PROBE, link_message_handler);
    if (ret == ESP_OK) {
        ret = EspNowLink::register_handler(MSG_CHANNEL_PROBE_RESPONSE, link_message_handler);
    }
    if (ret != ESP_OK) {
        EspNowLink::unregister_handler(MSG_CHANNEL_PROBE);
        vQueueDelete(event_queue);
        event_queue = nullptr;
        return ret;
    }
    if (xTaskCreate(pairing_task, "espnow_pair", TASK_STACK_SIZE, nullptr, TASK_PRIORITY, &task_handle) != pdPASS) {
        EspNowLink::unregister_handler(MSG_CHANNEL_PROBE);
        EspNowLink::unregister_handler(MSG_CHANNEL_PROBE_RESPONSE);
        vQueueDelete(event_queue);
        event_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }
    initialized = true;
    return ESP_OK;
}

esp_err_t enter_pairing_mode(uint32_t timeout_ms) {
    using namespace Internal;
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!EspNowLink::is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pairing_mode) {
        register_responder_handlers();
    }
    pairing_mode            = true;
    pairing_has_deadline    = timeout_ms != 0;
    pairing_deadline        = pairing_has_deadline ? xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms) : 0;
    active_nonce            = 0;
    pair_transaction_active = false;
    pending_peer            = {};
    memset(pending_lmk, 0, sizeof(pending_lmk));
    DEVICE_STATE_I(TAG, "espnow: pairing old=stopped new=active timeout_ms=%lu result=ok",
                   static_cast<uint32_t>(timeout_ms));
    return ESP_OK;
}

void leave_pairing_mode() {
    using namespace Internal;
    if (!pairing_mode) {
        return;
    }
    pairing_mode         = false;
    pairing_has_deadline = false;
    unregister_responder_handlers();
    if (pair_transaction_active) {
        EspNowLink::remove_peer(pending_peer);
        restore_peers();
        pair_transaction_active = false;
    }
    DEVICE_STATE_I(TAG, "espnow: pairing old=active new=stopped result=ok");
}

bool is_pairing() {
    return Internal::pairing_mode || Internal::initiating_pairing;
}

esp_err_t start_pairing() {
    using namespace Internal;
    if (!initialized || initiating_pairing || pairing_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!EspNowLink::is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    PairEvent event = {};
    event.type      = PairEventType::START_PAIRING;
    return xQueueSend(event_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t recover_peer_channel(const MacAddress& peer) {
    using namespace Internal;
    if (!initialized || channel_recovering || initiating_pairing || !has_peer(peer) || !is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    PairEvent event         = {};
    event.type              = PairEventType::START_CHANNEL_RECOVERY;
    event.source            = peer;
    channel_recovery_result = ESP_ERR_INVALID_STATE;
    channel_recovering      = true;
    if (xQueueSend(event_queue, &event, 0) != pdTRUE) {
        channel_recovering = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool is_recovering_channel() {
    return Internal::channel_recovering;
}

esp_err_t get_channel_recovery_result() {
    return Internal::channel_recovery_result;
}

size_t get_saved_peer_count() {
    return Internal::saved_peer_count();
}

esp_err_t get_saved_peer(size_t index, SavedPeer* peer) {
    return Internal::read_saved_peer(index, peer);
}

esp_err_t remove_saved_peer(const EspNowLink::MacAddress& address) {
    EspNowLink::remove_peer(address);
    return Internal::erase_peer(address);
}

esp_err_t clear_saved_peers() {
    const size_t previous_count = Internal::saved_peer_count();
    for (size_t i = 0; i < Internal::MAX_SAVED_PEERS; ++i) {
        SavedPeer peer = {};
        if (Internal::read_saved_peer(0, &peer) != ESP_OK) {
            break;
        }
        EspNowLink::remove_peer(peer.address);
        Internal::erase_peer(peer.address);
    }
    DEVICE_STATE_W(Internal::TAG, "espnow: peers action=clear old_count=%u new_count=0 result=ok",
                   static_cast<uint32_t>(previous_count));
    return ESP_OK;
}

} // namespace EspNowLink

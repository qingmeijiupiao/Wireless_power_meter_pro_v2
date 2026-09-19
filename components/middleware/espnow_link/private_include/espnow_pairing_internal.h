#ifndef ESPNOW_PAIRING_INTERNAL_H
#define ESPNOW_PAIRING_INTERNAL_H

#include "espnow_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace EspNowLink::Internal {

constexpr uint16_t MSG_CHANNEL_PROBE          = 0x0001;
constexpr uint16_t MSG_CHANNEL_PROBE_RESPONSE = 0x0002;
constexpr uint16_t MSG_DISCOVERY_PING         = 0x0100;
constexpr uint16_t MSG_DISCOVERY_RESPONSE     = 0x0101;
constexpr uint16_t MSG_PAIR_REQUEST           = 0x0102;
constexpr uint16_t MSG_PAIR_RESPONSE          = 0x0103;
constexpr uint16_t MSG_PAIR_CONFIRM           = 0x0104;
constexpr size_t   MAX_SAVED_PEERS            = 3;
constexpr uint32_t STORE_MAGIC                = 0x4e4f5750;
constexpr uint8_t  STORE_VERSION              = 1;

struct StoredPeer {
    uint8_t used;
    uint8_t reserved_role; /**< 存储布局占位，不参与链路逻辑。 */
    uint8_t mac[MAC_ADDRESS_SIZE];
    uint8_t lmk[KEY_SIZE];
    uint8_t last_channel;
    uint8_t reserved[3];
};

struct PeerStore {
    uint32_t   magic;
    uint8_t    version;
    uint8_t    count;
    uint8_t    reserved[2];
    StoredPeer peers[MAX_SAVED_PEERS];
    uint32_t   checksum;
};

enum class PairEventType : uint8_t {
    START_PAIRING,
    START_CHANNEL_RECOVERY,
    DISCOVERY_PING,
    DISCOVERY_RESPONSE,
    PAIR_REQUEST,
    PAIR_RESPONSE,
    PAIR_CONFIRM,
    PAIR_RESPONSE_SENT,
    CONFIRM_RESULT,
    CHANNEL_PROBE,
    CHANNEL_PROBE_RESPONSE,
};

struct PairEvent {
    PairEventType type;
    MacAddress    source;
    uint32_t      nonce;
    uint8_t       channel;
    uint8_t       lmk[KEY_SIZE];
    SendResult    send_result;
};

/** @brief 计算 peer 持久化表校验值。 */
uint32_t  calculate_checksum(const PeerStore& store);
/** @brief 从 NVS 读取并校验 peer 持久化表。 */
PeerStore load_store();
/** @brief 保存或更新一个已配对 peer。 */
esp_err_t save_peer(const PeerConfig& peer, uint8_t channel);
/** @brief 更新已保存 peer 的最近信道。 */
esp_err_t update_peer_channel(const MacAddress& address, uint8_t channel);
/** @brief 删除指定已保存 peer。 */
esp_err_t erase_peer(const MacAddress& address);
/** @brief 删除全部已保存 peer。 */
esp_err_t erase_all_peers();
/** @brief 返回有效的已保存 peer 数量。 */
size_t    saved_peer_count();
/** @brief 按逻辑索引读取一个已保存 peer。 */
esp_err_t read_saved_peer(size_t index, SavedPeer* output);
/** @brief 将 NVS 中的 peer 恢复到运行期链路。 */
void      restore_peers();

/** @brief 解码仅包含 nonce 的配对消息。 */
bool   decode_nonce(const Message& message, uint32_t* nonce);
/** @brief 解码配对响应中的 nonce 和 LMK。 */
bool   decode_pair_response(const Message& message, uint32_t* nonce, uint8_t lmk[KEY_SIZE]);
/** @brief 编码仅包含 nonce 的配对消息。 */
size_t encode_nonce(uint32_t nonce, uint8_t* output, size_t capacity);
/** @brief 编码发现响应。 */
size_t encode_discovery_response(const MacAddress& target, uint32_t nonce, uint8_t* output, size_t capacity);
/** @brief 解码并校验发现响应。 */
bool   decode_discovery_response(const Message& message, const MacAddress& local, uint32_t* nonce);
/** @brief 编码包含 LMK 的配对响应。 */
size_t encode_pair_response(uint32_t nonce, const uint8_t lmk[KEY_SIZE], uint8_t* output, size_t capacity);

/** @brief 初始化配对事件队列和后台任务。 */
esp_err_t init_pairing();

} // namespace EspNowLink::Internal

#endif

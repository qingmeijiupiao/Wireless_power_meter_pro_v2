/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: WiFi/Web/ESP-NOW 射频策略服务，负责 NVS、STA/AP/ESPNOW_ONLY 与配网
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-06 22:00:57
 */
#include "wifi_service.h"

#include <cstdio>
#include <cstring>

#include "HXC_NVS.h"
#include "dns_server.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "espnow_link.h"
#include "espnow_service.h"
#include "diagnostic_log.h"
#include "web_server.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "global_state.h"
#include "time_service.h"

namespace WifiService {

static constexpr char     TAG[]                      = "WifiService";
static constexpr uint8_t  STA_CONNECT_MAX_ATTEMPTS   = 2;
static constexpr size_t   RECONNECT_QUEUE_LENGTH     = 8;
static constexpr uint32_t RECONNECT_TASK_STACK_SIZE  = 3072;
static constexpr uint32_t RECONNECT_MAX_DELAY_MS     = 30000;
static constexpr uint32_t RECONNECT_INITIAL_DELAY_MS = 1000;

enum class ReconnectEvent : uint8_t {
    STA_DISCONNECTED,
    STA_GOT_IP,
    RETRY_TIMER,
};

static const char* source_or_unknown(const char* source) {
    return source == nullptr ? "unknown" : source;
}

// NVS key 长度需小于 16 字节，存储于 HXC 默认命名空间。
static HXC::NVS_DATA<char*>   sta_ssid("wifi_ssid", "");
static HXC::NVS_DATA<char*>   sta_pass("wifi_pass", "");
static HXC::NVS_DATA<uint8_t> web_boot("web_boot", 1);
static HXC::NVS_DATA<uint8_t> espnow_channel("espnow_ch", 1);

// 运行期状态由 WiFiService 统一维护，供 Shell 和 WebBackend 查询。
static bool              initialized                    = false;
static Mode              mode                           = Mode::OFF;
static char              ap_ssid[WIFI_SSID_MAX_LEN + 1] = {};
static char              last_error[64]                 = "none";
static SemaphoreHandle_t scan_mutex                     = nullptr;
static QueueHandle_t     reconnect_queue                = nullptr;
static TimerHandle_t     reconnect_timer                = nullptr;
static TaskHandle_t      reconnect_task_handle          = nullptr;
static bool              reconnect_enabled              = false;
static uint8_t           reconnect_attempt              = 0;

static esp_err_t persist_sta_credentials(const char* ssid, const char* password) {
    char  previous_ssid[WIFI_SSID_MAX_LEN + 1]         = {};
    char  previous_password[WIFI_PASSWORD_MAX_LEN + 1] = {};
    char* cached_ssid                                  = sta_ssid.read();
    char* cached_password                              = sta_pass.read();
    if (cached_ssid != nullptr) {
        strncpy(previous_ssid, cached_ssid, sizeof(previous_ssid) - 1);
    }
    if (cached_password != nullptr) {
        strncpy(previous_password, cached_password, sizeof(previous_password) - 1);
    }

    esp_err_t err = sta_ssid.set(ssid);
    if (err != ESP_OK) {
        return err;
    }
    err = sta_pass.set(password);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    const esp_err_t password_rollback_err = sta_pass.set(previous_password);
    const esp_err_t ssid_rollback_err     = sta_ssid.set(previous_ssid);
    if (password_rollback_err != ESP_OK || ssid_rollback_err != ESP_OK) {
        ESP_LOGE(TAG, "failed to rollback STA credentials: password=%s ssid=%s", esp_err_to_name(password_rollback_err),
                 esp_err_to_name(ssid_rollback_err));
    }
    return err;
}

static void update_global_state_flags() {
    const bool service_initialized = initialized;
    const bool wifi_enabled        = mode != Mode::OFF;
    const bool sta_connected       = mode == Mode::STA && WiFiManager::instance().is_connected();
    const bool ap_mode             = mode == Mode::AP_PROVISION;
    const bool saved_sta           = has_saved_sta();
    const bool web_on_boot         = is_web_enabled_on_boot();
    update_global_state([=](GlobalState& state) {
        state.flags.wifi_service_initialized = service_initialized;
        state.flags.wifi_enabled             = wifi_enabled;
        state.flags.wifi_sta_connected       = sta_connected;
        state.flags.wifi_ap_mode             = ap_mode;
        state.flags.wifi_has_saved_sta       = saved_sta;
        state.flags.wifi_web_enabled_on_boot = web_on_boot;
    });
}

static void enqueue_reconnect_event(ReconnectEvent event) {
    if (reconnect_queue == nullptr || xQueueSend(reconnect_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "reconnect queue full, dropping event=%u", static_cast<uint32_t>(event));
    }
}

/**
 * @brief 将底层断线和获取 IP 事件转发给重连任务
 *
 * 系统事件循环中只执行非阻塞入队，退避计算、定时器操作和日志记录均由后台任务处理。
 */
static void network_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void*) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        enqueue_reconnect_event(ReconnectEvent::STA_DISCONNECTED);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        enqueue_reconnect_event(ReconnectEvent::STA_GOT_IP);
    }
}

static uint32_t reconnect_delay_ms(uint8_t attempt) {
    uint32_t delay_ms = RECONNECT_INITIAL_DELAY_MS;
    while (attempt > 0 && delay_ms < RECONNECT_MAX_DELAY_MS) {
        delay_ms *= 2;
        attempt--;
    }
    return delay_ms > RECONNECT_MAX_DELAY_MS ? RECONNECT_MAX_DELAY_MS : delay_ms;
}

/** @brief 取消旧的退避计划，用于显式停止、切换 AP 或重新配置 STA。 */
static void cancel_reconnect() {
    reconnect_enabled = false;
    reconnect_attempt = 0;
    if (reconnect_timer != nullptr) {
        xTimerStop(reconnect_timer, 0);
    }
}

/**
 * @brief 按 1s、2s、4s、8s、16s、30s 封顶的指数退避计划下一次重连
 */
static void schedule_reconnect() {
    if (!reconnect_enabled || mode != Mode::STA || WiFiManager::instance().is_connected()) {
        return;
    }
    if (xTimerIsTimerActive(reconnect_timer) == pdTRUE) {
        return;
    }

    const uint32_t delay_ms = reconnect_delay_ms(reconnect_attempt);
    if (reconnect_attempt < UINT8_MAX) {
        reconnect_attempt++;
    }
    if (xTimerChangePeriod(reconnect_timer, pdMS_TO_TICKS(delay_ms), 0) != pdPASS) {
        ESP_LOGW(TAG, "failed to schedule STA reconnect attempt=%u", static_cast<uint32_t>(reconnect_attempt));
        return;
    }
    ESP_LOGW(TAG, "STA disconnected, reconnect attempt=%u scheduled in %lu ms",
             static_cast<uint32_t>(reconnect_attempt), static_cast<uint32_t>(delay_ms));
}

static void reconnect_timer_callback(TimerHandle_t) {
    enqueue_reconnect_event(ReconnectEvent::RETRY_TIMER);
}

/**
 * @brief 消费网络事件并发起运行期 STA 自动重连
 *
 * 首次启动连接仍由 connect_sta() 同步处理；本任务只处理已经进入 STA 模式后的
 * 意外断线，避免与 AP 配网和手动操作互相竞争。
 */
static void reconnect_task(void*) {
    ReconnectEvent event = ReconnectEvent::STA_DISCONNECTED;
    while (true) {
        if (xQueueReceive(reconnect_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event == ReconnectEvent::STA_GOT_IP) {
            const bool restored = reconnect_attempt > 0;
            reconnect_attempt   = 0;
            xTimerStop(reconnect_timer, 0);
            update_global_state_flags();
            if (restored) {
                IP_t ip = WiFiManager::instance().get_ip();
                DEVICE_STATE_I(TAG,
                               "wifi: connection old=disconnected new=connected source=auto_reconnect ip=%u.%u.%u.%u",
                               ip.octet1, ip.octet2, ip.octet3, ip.octet4);
            }
            continue;
        }

        if (event == ReconnectEvent::STA_DISCONNECTED) {
            update_global_state_flags();
            schedule_reconnect();
            continue;
        }

        if (!reconnect_enabled || mode != Mode::STA || WiFiManager::instance().is_connected()) {
            continue;
        }

        ESP_LOGI(TAG, "starting STA reconnect attempt=%u", static_cast<uint32_t>(reconnect_attempt));
        const esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "STA reconnect request failed: %s", esp_err_to_name(ret));
            schedule_reconnect();
        }
    }
}

/**
 * @brief 更新最近一次错误描述
 *
 * @param err 错误字符串，nullptr 时记录为 "unknown"
 */
static void set_last_error(const char* err) {
    strncpy(last_error, err == nullptr ? "unknown" : err, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

/**
 * @brief 校验当前非 OFF 网络模式下 ESP-NOW 链路已经随 WiFi 射频启动。
 */
static esp_err_t require_espnow_active() {
    if (EspNowLink::is_active()) {
        return ESP_OK;
    }
    set_last_error("espnow inactive");
    ESP_LOGE(TAG, "ESP-NOW link did not activate with WiFi radio");
    return ESP_ERR_INVALID_STATE;
}

/**
 * @brief 基于 AP MAC 生成默认配网热点名
 *
 * 命名格式为 `WPM-PRO-V2-XXXXXX`，后缀取 AP MAC 的后三字节，便于用户区分多台设备。
 */
static void make_ap_ssid() {
    MAC_t mac = WiFiManager::instance().get_mac(WIFI_IF_AP);
    snprintf(ap_ssid, sizeof(ap_ssid), "WPM-PRO-V2-%02X%02X%02X", mac.octet4, mac.octet5, mac.octet6);
}

/**
 * @brief 将 AP netif IP 设置为 WiFiService 编译期常量
 */
static esp_err_t configure_ap_ip() {
    IP_t ip   = {};
    ip.octet1 = AP_IP_OCTET1;
    ip.octet2 = AP_IP_OCTET2;
    ip.octet3 = AP_IP_OCTET3;
    ip.octet4 = AP_IP_OCTET4;

    IP_t netmask   = {};
    netmask.octet1 = 255;
    netmask.octet2 = 255;
    netmask.octet3 = 255;
    netmask.octet4 = 0;

    return WiFiManager::instance().set_ap_ip(ip, netmask);
}

/**
 * @brief 初始化 WiFiService
 */
esp_err_t init() {
    if (initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(HXC::NVS_Base::setup(), TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(WiFiManager::instance().init(), TAG, "wifi manager init failed");
    ESP_RETURN_ON_ERROR(EspNowService::init(), TAG, "ESP-NOW service init failed");
    ESP_RETURN_ON_ERROR(TimeService::init(), TAG, "time service init failed");
    make_ap_ssid();
    scan_mutex = xSemaphoreCreateMutex();
    if (scan_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    reconnect_queue = xQueueCreate(RECONNECT_QUEUE_LENGTH, sizeof(ReconnectEvent));
    reconnect_timer = xTimerCreate("wifi_reconnect", pdMS_TO_TICKS(RECONNECT_INITIAL_DELAY_MS), pdFALSE, nullptr,
                                   reconnect_timer_callback);
    if (reconnect_queue == nullptr || reconnect_timer == nullptr) {
        if (reconnect_timer != nullptr) {
            xTimerDelete(reconnect_timer, 0);
            reconnect_timer = nullptr;
        }
        if (reconnect_queue != nullptr) {
            vQueueDelete(reconnect_queue);
            reconnect_queue = nullptr;
        }
        vSemaphoreDelete(scan_mutex);
        scan_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(reconnect_task, "wifi_reconnect", RECONNECT_TASK_STACK_SIZE, nullptr, 3, &reconnect_task_handle) !=
        pdPASS) {
        xTimerDelete(reconnect_timer, 0);
        vQueueDelete(reconnect_queue);
        vSemaphoreDelete(scan_mutex);
        reconnect_timer = nullptr;
        reconnect_queue = nullptr;
        scan_mutex      = nullptr;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, network_event_handler, nullptr);
    if (ret == ESP_OK) {
        ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event_handler, nullptr);
    }
    if (ret != ESP_OK) {
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, network_event_handler);
        vTaskDelete(reconnect_task_handle);
        xTimerDelete(reconnect_timer, 0);
        vQueueDelete(reconnect_queue);
        vSemaphoreDelete(scan_mutex);
        reconnect_task_handle = nullptr;
        reconnect_timer       = nullptr;
        reconnect_queue       = nullptr;
        scan_mutex            = nullptr;
        return ret;
    }
    initialized = true;
    update_global_state_flags();
    DEVICE_EVENT_I(TAG, "wifi: init web_boot=%u saved_sta=%u", is_web_enabled_on_boot() ? 1U : 0U,
                   has_saved_sta() ? 1U : 0U);
    return ESP_OK;
}

/**
 * @brief 查询初始化状态
 */
bool is_initialized() {
    return initialized;
}

/**
 * @brief 查询启动时是否启用 WiFi/Web
 */
bool is_web_enabled_on_boot() {
    return web_boot.read() != 0;
}

/**
 * @brief 写入启动启用开关
 */
esp_err_t set_web_enabled_on_boot(bool enabled, const char* source) {
    ESP_RETURN_ON_ERROR(web_boot.set(enabled ? 1 : 0), TAG, "persist web boot setting failed");
    update_global_state_flags();
    DEVICE_EVENT_I(TAG, "wifi: config source=%s web_boot=%u", source_or_unknown(source), enabled ? 1U : 0U);
    return ESP_OK;
}

/**
 * @brief 清除已保存 STA 凭据
 */
esp_err_t clear_saved_sta(const char* source) {
    ESP_RETURN_ON_ERROR(persist_sta_credentials("", ""), TAG, "clear saved STA credentials failed");
    update_global_state_flags();
    DEVICE_EVENT_I(TAG, "wifi: config source=%s saved_sta_cleared=1", source_or_unknown(source));
    return ESP_OK;
}

/**
 * @brief 判断 NVS 中是否存在有效 SSID
 */
bool has_saved_sta() {
    char* ssid = sta_ssid.read();
    return ssid != nullptr && ssid[0] != '\0';
}

/**
 * @brief 读取当前配置快照
 */
Config get_config() {
    Config cfg  = {};
    char*  ssid = sta_ssid.read();
    char*  pass = sta_pass.read();
    if (ssid != nullptr) {
        strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
    }
    if (pass != nullptr) {
        strncpy(cfg.password, pass, sizeof(cfg.password) - 1);
    }
    cfg.web_enabled_on_boot = is_web_enabled_on_boot();
    return cfg;
}

/**
 * @brief 连接指定 STA WiFi
 */
esp_err_t connect_sta(const char* ssid, const char* password, bool save, const char* source) {
    if (ssid == nullptr || ssid[0] == '\0' || password == nullptr) {
        set_last_error("invalid sta config");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(init(), TAG, "init failed");
    cancel_reconnect();

    // 从配网 AP 切换到 STA 前必须关闭 DNS 劫持和 Captive Portal 回落。
    DNSServer::stop();
    WebServer::enable_captive_portal(false);

    esp_err_t ret = ESP_FAIL;
    for (uint8_t attempt = 1; attempt <= STA_CONNECT_MAX_ATTEMPTS; ++attempt) {
        ESP_LOGI(TAG, "STA connect attempt %u/%u: ssid=%s", static_cast<uint32_t>(attempt),
                 static_cast<uint32_t>(STA_CONNECT_MAX_ATTEMPTS), ssid);
        ret = WiFiManager::instance().connect_sta(ssid, password, true);
        if (ret != ESP_ERR_TIMEOUT || attempt == STA_CONNECT_MAX_ATTEMPTS) {
            break;
        }
        ESP_LOGW(TAG, "STA connect attempt %u/%u timed out, retrying once", static_cast<uint32_t>(attempt),
                 static_cast<uint32_t>(STA_CONNECT_MAX_ATTEMPTS));
    }
    if (ret == ESP_OK) {
        ESP_RETURN_ON_ERROR(require_espnow_active(), TAG, "ESP-NOW link inactive");
        const Mode old_mode = mode;
        mode                = Mode::STA;
        reconnect_enabled   = true;
        update_global_state_flags();
        set_last_error("none");

        // 只有业务层明确要求保存时才写入 NVS，启动自动连接不会重复写 Flash。
        if (save) {
            ESP_RETURN_ON_ERROR(persist_sta_credentials(ssid, password), TAG, "persist STA credentials failed");
        }
        IP_t ip = WiFiManager::instance().get_ip();
        DEVICE_STATE_I(TAG, "wifi: mode old=%u new=%u source=%s result=ok ssid=%s save=%u ip=%u.%u.%u.%u",
                       static_cast<uint32_t>(old_mode), static_cast<uint32_t>(mode), source_or_unknown(source), ssid,
                       save ? 1U : 0U, ip.octet1, ip.octet2, ip.octet3, ip.octet4);
        return ESP_OK;
    }

    set_last_error(esp_err_to_name(ret));
    ESP_LOGW(TAG, "wifi: sta_connect source=%s ssid=%s save=%u result=failed err=%s", source_or_unknown(source), ssid,
             save ? 1U : 0U, esp_err_to_name(ret));
    return ret;
}

/**
 * @brief 启动开放 AP 配网模式
 */
esp_err_t start_provision_ap(const char* source) {
    ESP_RETURN_ON_ERROR(init(), TAG, "init failed");
    cancel_reconnect();
    if (ap_ssid[0] == '\0') {
        make_ap_ssid();
    }

    // AP 使用空密码，降低首次配网门槛；HTTP 404/Captive 请求回落到配网页。
    ESP_RETURN_ON_ERROR(configure_ap_ip(), TAG, "set ap ip failed");
    ESP_RETURN_ON_ERROR(WiFiManager::instance().start_apsta(ap_ssid, ""), TAG, "start apsta failed");
    ESP_RETURN_ON_ERROR(require_espnow_active(), TAG, "ESP-NOW link inactive");
    ESP_RETURN_ON_ERROR(DNSServer::start(AP_IP_OCTET1, AP_IP_OCTET2, AP_IP_OCTET3, AP_IP_OCTET4), TAG,
                        "start dns failed");
    WebServer::enable_captive_portal(true);
    const Mode old_mode = mode;
    mode                = Mode::AP_PROVISION;
    update_global_state_flags();
    set_last_error("none");
    DEVICE_STATE_I(TAG, "wifi: mode old=%u new=%u source=%s result=ok ssid=%s", static_cast<uint32_t>(old_mode),
                   static_cast<uint32_t>(mode), source_or_unknown(source), ap_ssid);
    return ESP_OK;
}

esp_err_t start_espnow_only(const char* source) {
    ESP_RETURN_ON_ERROR(init(), TAG, "init failed");
    cancel_reconnect();
    DNSServer::stop();
    WebServer::enable_captive_portal(false);

    uint8_t channel = espnow_channel.read();
    if (channel == 0 || channel > 14) {
        channel = 1;
        ESP_RETURN_ON_ERROR(espnow_channel.set(channel), TAG, "persist default ESP-NOW channel failed");
    }
    ESP_RETURN_ON_ERROR(WiFiManager::instance().start_sta_radio(channel), TAG, "start ESP-NOW-only radio failed");
    ESP_RETURN_ON_ERROR(require_espnow_active(), TAG, "ESP-NOW link inactive");
    const Mode old_mode = mode;
    mode                = Mode::ESPNOW_ONLY;
    update_global_state_flags();
    set_last_error("none");
    DEVICE_STATE_I(TAG, "wifi: mode old=%u new=%u source=%s result=ok channel=%u", static_cast<uint32_t>(old_mode),
                   static_cast<uint32_t>(mode), source_or_unknown(source), static_cast<uint32_t>(channel));
    return ESP_OK;
}

esp_err_t scan_ap_list(ScanResult* results, size_t max_results, size_t* out_count) {
    if (results == nullptr || out_count == nullptr || max_results == 0) {
        set_last_error("invalid scan args");
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    ESP_RETURN_ON_ERROR(init(), TAG, "init failed");

    if (scan_mutex == nullptr || xSemaphoreTake(scan_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        set_last_error("scan busy");
        return ESP_ERR_TIMEOUT;
    }

    wifi_scan_config_t scan_cfg   = {};
    scan_cfg.show_hidden          = false;
    scan_cfg.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 80;
    scan_cfg.scan_time.active.max = 180;

    esp_err_t ret = WiFiManager::instance().scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        set_last_error(esp_err_to_name(ret));
        xSemaphoreGive(scan_mutex);
        return ret;
    }

    uint16_t ap_num = 0;
    ret             = WiFiManager::instance().scan_get_ap_num(&ap_num);
    if (ret != ESP_OK) {
        set_last_error(esp_err_to_name(ret));
        xSemaphoreGive(scan_mutex);
        return ret;
    }

    wifi_ap_record_t records[WIFI_SCAN_MAX_RESULTS] = {};
    uint16_t         fetch_num                      = ap_num > WIFI_SCAN_MAX_RESULTS ? WIFI_SCAN_MAX_RESULTS : ap_num;
    ret                                             = WiFiManager::instance().scan_get_ap_records(&fetch_num, records);
    if (ret != ESP_OK) {
        set_last_error(esp_err_to_name(ret));
        xSemaphoreGive(scan_mutex);
        return ret;
    }

    size_t written = 0;
    for (uint16_t i = 0; i < fetch_num && written < max_results; ++i) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }

        bool duplicated = false;
        for (size_t j = 0; j < written; ++j) {
            if (strncmp(results[j].ssid, reinterpret_cast<const char*>(records[i].ssid), WIFI_SSID_MAX_LEN) == 0) {
                duplicated = true;
                break;
            }
        }
        if (duplicated) {
            continue;
        }

        strncpy(results[written].ssid, reinterpret_cast<const char*>(records[i].ssid), WIFI_SSID_MAX_LEN);
        results[written].ssid[WIFI_SSID_MAX_LEN] = '\0';
        results[written].rssi                    = records[i].rssi;
        results[written].channel                 = records[i].primary;
        results[written].authmode                = records[i].authmode;
        written++;
    }

    *out_count = written;
    set_last_error("none");
    xSemaphoreGive(scan_mutex);
    return ESP_OK;
}

/**
 * @brief 按 NVS 配置启动默认模式
 */
esp_err_t start_default(const char* source) {
    ESP_RETURN_ON_ERROR(init(), TAG, "init failed");

    if (!is_web_enabled_on_boot()) {
        DEVICE_EVENT_I(TAG, "wifi: startup source=%s web_enabled=0 fallback=espnow_only", source_or_unknown(source));
        return start_espnow_only(source);
    }

    Config cfg = get_config();
    if (cfg.ssid[0] != '\0') {
        esp_err_t ret = connect_sta(cfg.ssid, cfg.password, false, source);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
    }

    // 没有保存凭据，或保存的凭据连接超时/失败时，进入 AP 配网兜底模式。
    return start_provision_ap(source);
}

/**
 * @brief 关闭 IP 网络并保留 ESP-NOW 射频
 */
esp_err_t stop(const char* source) {
    cancel_reconnect();
    DNSServer::stop();
    WebServer::enable_captive_portal(false);
    esp_err_t ret = start_espnow_only(source);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi: web_stop source=%s fallback=espnow_only result=%s", source_or_unknown(source),
                 esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 查询是否处于 AP 配网模式
 */
bool is_provisioning() {
    return mode == Mode::AP_PROVISION;
}

/**
 * @brief 获取当前模式
 */
Mode get_mode() {
    return mode;
}

/**
 * @brief 获取配网热点名
 */
const char* get_ap_ssid() {
    if (ap_ssid[0] == '\0') {
        make_ap_ssid();
    }
    return ap_ssid;
}

/**
 * @brief 获取最近错误描述
 */
const char* get_last_error() {
    return last_error;
}

/**
 * @brief 获取当前对外访问 IP
 */
IP_t get_ip() {
    if (mode == Mode::AP_PROVISION) {
        IP_t ip   = {};
        ip.octet1 = AP_IP_OCTET1;
        ip.octet2 = AP_IP_OCTET2;
        ip.octet3 = AP_IP_OCTET3;
        ip.octet4 = AP_IP_OCTET4;
        return ip;
    }
    return WiFiManager::instance().get_ip();
}

/**
 * @brief 获取底层 WiFi 状态
 */
wifi_state_t get_wifi_state() {
    return WiFiManager::instance().get_state();
}

/**
 * @brief 获取当前 STA 连接的 RSSI
 */
int8_t get_rssi() {
    return WiFiManager::instance().get_rssi();
}

/**
 * @brief 将当前 STA RSSI 映射为便于展示的信号强度百分比
 */
uint8_t get_signal_percent() {
    if (get_wifi_state() != WIFI_STATE_STA_CONNECTED) {
        return 0;
    }

    int8_t rssi = get_rssi();
    if (rssi <= -100) {
        return 0;
    }
    if (rssi >= -50) {
        return 100;
    }
    return static_cast<uint8_t>(2 * (rssi + 100));
}

/**
 * @brief 获取当前 WiFi 主信道
 */
esp_err_t get_channel(uint8_t* channel) {
    if (channel == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    return WiFiManager::instance().get_channel(channel, &second);
}

esp_err_t set_espnow_channel(uint8_t channel) {
    if (channel == 0 || channel > 14) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(espnow_channel.set(channel), TAG, "persist ESP-NOW channel failed");
    if (mode == Mode::ESPNOW_ONLY) {
        return WiFiManager::instance().set_channel(channel);
    }
    return ESP_OK;
}

} // namespace WifiService

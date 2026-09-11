/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: WiFi管理单例类实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-04-20 22:37:09
 */
#include "wifi_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "dhcpserver/dhcpserver.h"
#include "lwip/ip_addr.h"
#include <cstring>

static const char* TAG = "WiFiManager";

/* 事件组位定义 */
#define WIFI_CONNECTED_BIT BIT0  /**< STA已连接并获取IP */
#define WIFI_FAIL_BIT BIT1       /**< STA连接失败/断开 */
#define WIFI_SCAN_DONE_BIT BIT2  /**< 扫描完成 */
#define WIFI_ASSOCIATED_BIT BIT3 /**< 本次STA连接尝试已完成802.11关联 */

/* ==================== 构造/析构 ==================== */

WiFiManager::WiFiManager()
    : state_(WIFI_STATE_DISCONNECTED), initialized_(false), wifi_started_(false), sta_netif_(nullptr),
      ap_netif_(nullptr), event_group_(nullptr) {
    memset(&ip_, 0, sizeof(ip_));
    memset(radio_listeners_, 0, sizeof(radio_listeners_));
}

WiFiManager::~WiFiManager() {
    deinit();
}

/* ==================== 初始化/反初始化 ==================== */

esp_err_t WiFiManager::init() {
    if (initialized_) {
        return ESP_OK;
    }

    /* 1. 初始化TCP/IP协议栈 */
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 创建默认事件循环 */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 创建STA和AP的netif实例 */
    sta_netif_ = esp_netif_create_default_wifi_sta();
    ap_netif_  = esp_netif_create_default_wifi_ap();

    /* 4. 使用默认配置初始化WiFi驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret                    = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5. 注册WiFi事件回调 */
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, this, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register wifi event handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 6. 注册IP事件回调 */
    ret = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, this, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register ip event handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 7. 创建事件组，用于连接/扫描同步 */
    event_group_ = xEventGroupCreate();
    if (event_group_ == nullptr) {
        ESP_LOGE(TAG, "create event group failed");
        return ESP_ERR_NO_MEM;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "WiFiManager initialized");
    return ESP_OK;
}

esp_err_t WiFiManager::deinit() {
    if (!initialized_) {
        return ESP_OK;
    }

    /* 先停止WiFi */
    stop();

    /* 释放事件组 */
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }

    /* 反初始化WiFi驱动 */
    esp_wifi_deinit();

    /* 销毁netif实例 */
    if (sta_netif_ != nullptr) {
        esp_netif_destroy(sta_netif_);
        sta_netif_ = nullptr;
    }
    if (ap_netif_ != nullptr) {
        esp_netif_destroy(ap_netif_);
        ap_netif_ = nullptr;
    }

    initialized_ = false;
    ESP_LOGI(TAG, "WiFiManager deinitialized");
    return ESP_OK;
}

/* ==================== STA模式连接 ==================== */

esp_err_t WiFiManager::connect_sta(const char* ssid, const char* password, bool wait) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 如果WiFi已在运行，先停止 */
    if (wifi_started_) {
        stop_wifi_driver();
    }

    /* 设置STA模式 */
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set STA mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 配置SSID和密码 */
    wifi_config_t sta_config = {};
    strncpy(reinterpret_cast<char*>(sta_config.sta.ssid), ssid, WIFI_SSID_MAX_LEN - 1);
    strncpy(reinterpret_cast<char*>(sta_config.sta.password), password, WIFI_PASSWORD_MAX_LEN - 1);
    /* 空密码使用OPEN认证，否则使用WPA2 */
    sta_config.sta.threshold.authmode = (strlen(password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 清除之前的事件标志 */
    xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_ASSOCIATED_BIT);

    /* 启动WiFi驱动 */
    ret = start_wifi_driver();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* 发起连接 */
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Connecting to SSID:%s...", ssid);

    /* 可选：阻塞等待连接结果 */
    if (wait) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to AP SSID:%s", ssid);
            return ESP_OK;
        } else if (bits & WIFI_FAIL_BIT) {
            if (bits & WIFI_ASSOCIATED_BIT) {
                ESP_LOGW(TAG, "Connect to SSID:%s failed after AP association while waiting for DHCP IP", ssid);
            } else {
                ESP_LOGW(TAG, "Connect to SSID:%s failed before AP association", ssid);
            }
        } else {
            if (bits & WIFI_ASSOCIATED_BIT) {
                ESP_LOGW(TAG,
                         "Connect to SSID:%s timed out: associated with AP but DHCP IP was not acquired within %d ms",
                         ssid, WIFI_CONNECT_TIMEOUT_MS);
            } else {
                ESP_LOGW(TAG, "Connect to SSID:%s timed out before AP association within %d ms", ssid,
                         WIFI_CONNECT_TIMEOUT_MS);
            }
        }
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t WiFiManager::start_sta_radio(uint8_t channel) {
    if (!initialized_ || channel == 0 || channel > 14) {
        return !initialized_ ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    }
    if (wifi_started_) {
        ESP_RETURN_ON_ERROR(stop_wifi_driver(), TAG, "stop WiFi before STA radio start failed");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
    ESP_RETURN_ON_ERROR(start_wifi_driver(), TAG, "start STA radio failed");
    esp_err_t ret = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (ret != ESP_OK) {
        stop_wifi_driver();
        return ret;
    }
    state_ = WIFI_STATE_DISCONNECTED;
    ESP_LOGI(TAG, "STA radio started on channel %u", static_cast<uint32_t>(channel));
    return ESP_OK;
}

/* ==================== AP模式启动 ==================== */

esp_err_t WiFiManager::start_ap(const char* ssid, const char* password, uint8_t max_conn, uint8_t channel) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 如果WiFi已在运行，先停止 */
    if (wifi_started_) {
        stop_wifi_driver();
    }

    /* 设置AP模式 */
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set AP mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 配置AP参数 */
    wifi_config_t ap_config = {};
    strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), ssid, WIFI_SSID_MAX_LEN - 1);
    strncpy(reinterpret_cast<char*>(ap_config.ap.password), password, WIFI_PASSWORD_MAX_LEN - 1);
    ap_config.ap.ssid_len       = static_cast<uint8_t>(strlen(ssid));
    ap_config.ap.channel        = channel;
    ap_config.ap.max_connection = max_conn;
    /* 空密码使用OPEN认证，否则使用WPA2 */
    ap_config.ap.authmode       = (strlen(password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 启动WiFi驱动 */
    ret = start_wifi_driver();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    state_ = WIFI_STATE_AP_ACTIVE;
    ESP_LOGI(TAG, "AP started SSID:%s Channel:%d", ssid, channel);
    return ESP_OK;
}

esp_err_t WiFiManager::start_apsta(const char* ssid, const char* password, uint8_t max_conn, uint8_t channel) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    if (wifi_started_) {
        stop_wifi_driver();
    }

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set APSTA mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t ap_config = {};
    strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), ssid, WIFI_SSID_MAX_LEN - 1);
    strncpy(reinterpret_cast<char*>(ap_config.ap.password), password, WIFI_PASSWORD_MAX_LEN - 1);
    ap_config.ap.ssid_len       = static_cast<uint8_t>(strlen(ssid));
    ap_config.ap.channel        = channel;
    ap_config.ap.max_connection = max_conn;
    ap_config.ap.authmode       = (strlen(password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = start_wifi_driver();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    state_ = WIFI_STATE_AP_ACTIVE;
    ESP_LOGI(TAG, "APSTA started SSID:%s Channel:%d", ssid, channel);
    return ESP_OK;
}

/* ==================== 断开/停止 ==================== */

esp_err_t WiFiManager::disconnect() {
    if (!initialized_ || !wifi_started_) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 仅在STA/APSTA模式下执行断开操作 */
    wifi_mode_t mode;
    esp_err_t   ret = esp_wifi_get_mode(&mode);
    if (ret == ESP_OK && (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA)) {
        ret = esp_wifi_disconnect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "wifi disconnect failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* 更新状态 */
    state_ = WIFI_STATE_DISCONNECTED;
    memset(&ip_, 0, sizeof(ip_));
    xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT);
    xEventGroupSetBits(event_group_, WIFI_FAIL_BIT);

    ESP_LOGI(TAG, "WiFi disconnected");
    return ESP_OK;
}

esp_err_t WiFiManager::stop() {
    esp_err_t ret = stop_wifi_driver();
    if (ret == ESP_OK) {
        state_ = WIFI_STATE_DISCONNECTED;
        memset(&ip_, 0, sizeof(ip_));
        xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_ASSOCIATED_BIT);
        ESP_LOGI(TAG, "WiFi stopped");
    }
    return ret;
}

/* ==================== 状态查询 ==================== */

wifi_state_t WiFiManager::get_state() const {
    return state_;
}

IP_t WiFiManager::get_ip() const {
    return ip_;
}

IP_t WiFiManager::get_ap_ip() const {
    IP_t ip = {};
    if (ap_netif_ == nullptr) {
        return ip;
    }

    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(ap_netif_, &ip_info) == ESP_OK) {
        ip.addr = ip_info.ip.addr;
    }
    return ip;
}

MAC_t WiFiManager::get_mac(wifi_interface_t ifx) const {
    MAC_t mac = {};
    if (initialized_) {
        esp_wifi_get_mac(ifx, mac.bytes);
    }
    return mac;
}

int8_t WiFiManager::get_rssi() const {
    if (state_ != WIFI_STATE_STA_CONNECTED) {
        return 0;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

bool WiFiManager::is_connected() const {
    return state_ == WIFI_STATE_STA_CONNECTED;
}

bool WiFiManager::is_initialized() const {
    return initialized_;
}

bool WiFiManager::is_started() const {
    return wifi_started_;
}

esp_err_t WiFiManager::register_radio_listener(RadioEventHandler handler, void* context) {
    if (handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const auto& listener : radio_listeners_) {
        if (listener.handler == handler && listener.context == context) {
            return ESP_OK;
        }
    }
    for (auto& listener : radio_listeners_) {
        if (listener.handler == nullptr) {
            listener = {handler, context};
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t WiFiManager::unregister_radio_listener(RadioEventHandler handler, void* context) {
    for (auto& listener : radio_listeners_) {
        if (listener.handler == handler && listener.context == context) {
            listener = {};
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ==================== 扫描功能 ==================== */

esp_err_t WiFiManager::scan_start(const wifi_scan_config_t* config, bool block) {
    if (!initialized_ || !wifi_started_) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(event_group_, WIFI_SCAN_DONE_BIT);

    /* 若未提供配置则使用全零默认配置 */
    wifi_scan_config_t        default_config = {};
    const wifi_scan_config_t* cfg            = config ? config : &default_config;

    esp_err_t ret = esp_wifi_scan_start(cfg, block);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (block) {
        xEventGroupSetBits(event_group_, WIFI_SCAN_DONE_BIT);
    }
    return ESP_OK;
}

esp_err_t WiFiManager::scan_stop() {
    return esp_wifi_scan_stop();
}

esp_err_t WiFiManager::scan_get_ap_num(uint16_t* num) {
    return esp_wifi_scan_get_ap_num(num);
}

esp_err_t WiFiManager::scan_get_ap_records(uint16_t* num, wifi_ap_record_t* records) {
    return esp_wifi_scan_get_ap_records(num, records);
}

/* ==================== 省电/信道/带宽/协议 ==================== */

esp_err_t WiFiManager::set_power_save(wifi_ps_type_t type) {
    return esp_wifi_set_ps(type);
}

esp_err_t WiFiManager::get_power_save(wifi_ps_type_t* type) {
    return esp_wifi_get_ps(type);
}

esp_err_t WiFiManager::set_channel(uint8_t primary, wifi_second_chan_t second) {
    return esp_wifi_set_channel(primary, second);
}

esp_err_t WiFiManager::get_channel(uint8_t* primary, wifi_second_chan_t* second) {
    return esp_wifi_get_channel(primary, second);
}

esp_err_t WiFiManager::set_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t bw) {
    return esp_wifi_set_bandwidth(ifx, bw);
}

esp_err_t WiFiManager::get_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t* bw) {
    return esp_wifi_get_bandwidth(ifx, bw);
}

esp_err_t WiFiManager::set_protocol(wifi_interface_t ifx, uint8_t protocol_bitmap) {
    return esp_wifi_set_protocol(ifx, protocol_bitmap);
}

esp_err_t WiFiManager::get_protocol(wifi_interface_t ifx, uint8_t* protocol_bitmap) {
    return esp_wifi_get_protocol(ifx, protocol_bitmap);
}

/* ==================== 发射功率 ==================== */

esp_err_t WiFiManager::set_max_tx_power(int8_t power) {
    return esp_wifi_set_max_tx_power(power);
}

esp_err_t WiFiManager::get_max_tx_power(int8_t* power) {
    return esp_wifi_get_max_tx_power(power);
}

/* ==================== 国家/地区 ==================== */

esp_err_t WiFiManager::set_country(const wifi_country_t* country) {
    return esp_wifi_set_country(country);
}

esp_err_t WiFiManager::get_country(wifi_country_t* country) {
    return esp_wifi_get_country(country);
}

/* ==================== AP模式STA管理 ==================== */

esp_err_t WiFiManager::ap_get_sta_list(wifi_sta_list_t* sta_list) {
    return esp_wifi_ap_get_sta_list(sta_list);
}

esp_err_t WiFiManager::ap_deauth_sta(uint16_t aid) {
    return esp_wifi_deauth_sta(aid);
}

/* ==================== 配置读写 ==================== */

esp_err_t WiFiManager::set_sta_config(const char* ssid, const char* password, bool bssid_set, const uint8_t* bssid) {
    wifi_config_t conf = {};
    strncpy(reinterpret_cast<char*>(conf.sta.ssid), ssid, WIFI_SSID_MAX_LEN - 1);
    strncpy(reinterpret_cast<char*>(conf.sta.password), password, WIFI_PASSWORD_MAX_LEN - 1);
    conf.sta.bssid_set = bssid_set;
    if (bssid_set && bssid != nullptr) {
        memcpy(conf.sta.bssid, bssid, 6);
    }
    conf.sta.threshold.authmode = (strlen(password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_STA, &conf);
}

esp_err_t WiFiManager::get_sta_config(wifi_config_t* conf) {
    return esp_wifi_get_config(WIFI_IF_STA, conf);
}

esp_err_t WiFiManager::set_ap_config(const char* ssid, const char* password, uint8_t channel, wifi_auth_mode_t authmode,
                                     uint8_t max_conn) {
    wifi_config_t conf = {};
    strncpy(reinterpret_cast<char*>(conf.ap.ssid), ssid, WIFI_SSID_MAX_LEN - 1);
    strncpy(reinterpret_cast<char*>(conf.ap.password), password, WIFI_PASSWORD_MAX_LEN - 1);
    conf.ap.ssid_len       = static_cast<uint8_t>(strlen(ssid));
    conf.ap.channel        = channel;
    conf.ap.authmode       = authmode;
    conf.ap.max_connection = max_conn;
    return esp_wifi_set_config(WIFI_IF_AP, &conf);
}

esp_err_t WiFiManager::get_ap_config(wifi_config_t* conf) {
    return esp_wifi_get_config(WIFI_IF_AP, conf);
}

esp_err_t WiFiManager::set_ap_ip(IP_t ip, IP_t netmask) {
    if (!initialized_ || ap_netif_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info = {};
    ip_info.ip.addr             = ip.addr;
    ip_info.gw.addr             = ip.addr;
    ip_info.netmask.addr        = netmask.addr;

    esp_err_t ret = esp_netif_dhcps_stop(ap_netif_);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return ret;
    }

    ret = esp_netif_set_ip_info(ap_netif_, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }

    // Captive Portal 必须通过 DHCP Option 6 明确告诉客户端使用本机 DNS。
    // 仅在 UDP 53 端口启动 DNS 服务并不会改变手机获得的 DNS 地址。
    esp_netif_dns_info_t dns_info = {};
    dns_info.ip.type              = IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4.addr   = ip.addr;
    ret = esp_netif_set_dns_info(ap_netif_, ESP_NETIF_DNS_MAIN, &dns_info);
    if (ret != ESP_OK) {
        return ret;
    }

    dhcps_offer_t dns_offer = OFFER_DNS;
    ret = esp_netif_dhcps_option(ap_netif_, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dns_offer,
                                 sizeof(dns_offer));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_netif_dhcps_start(ap_netif_);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        return ret;
    }
    ESP_LOGI(TAG, "AP DHCP DNS set to " IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

/* ==================== MAC地址设置 ==================== */

esp_err_t WiFiManager::set_mac(wifi_interface_t ifx, const uint8_t mac[6]) {
    return esp_wifi_set_mac(ifx, mac);
}

/* ==================== 内部辅助 ==================== */

void WiFiManager::update_ip_from_netif() {
    if (sta_netif_ == nullptr) {
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta_netif_, &ip_info) == ESP_OK) {
        ip_.addr = ip_info.ip.addr;
    }
}

esp_err_t WiFiManager::start_wifi_driver() {
    if (wifi_started_) {
        return ESP_OK;
    }
    esp_err_t ret = esp_wifi_start();
    if (ret == ESP_OK) {
        wifi_started_ = true;
        notify_radio_event(RadioEvent::AFTER_START);
    }
    return ret;
}

esp_err_t WiFiManager::stop_wifi_driver() {
    if (!wifi_started_) {
        return ESP_OK;
    }
    notify_radio_event(RadioEvent::BEFORE_STOP);
    esp_err_t ret = esp_wifi_stop();
    if (ret == ESP_OK) {
        wifi_started_ = false;
    }
    return ret;
}

void WiFiManager::notify_radio_event(RadioEvent event) {
    for (const auto& listener : radio_listeners_) {
        if (listener.handler != nullptr) {
            listener.handler(event, listener.context);
        }
    }
}

/* ==================== 事件回调 ==================== */

/**
 * @brief WiFi事件回调
 *
 * 处理STA连接/断开、AP启动/停止、STA加入/离开AP、扫描完成等事件。
 * 通过arg参数获取WiFiManager实例指针以更新内部状态。
 */
void WiFiManager::wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    WiFiManager* self = static_cast<WiFiManager*>(arg);

    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
    /* STA启动 */
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started");
        break;

    /* STA已关联到AP，等待DHCP分配IP */
    case WIFI_EVENT_STA_CONNECTED:
        xEventGroupSetBits(self->event_group_, WIFI_ASSOCIATED_BIT);
        ESP_LOGI(TAG, "STA associated with AP, waiting for DHCP IP...");
        break;

    /* STA断开连接，更新状态并设置失败标志 */
    case WIFI_EVENT_STA_DISCONNECTED: {
        ESP_LOGW(TAG, "STA disconnected");
        self->state_ = WIFI_STATE_DISCONNECTED;
        memset(&self->ip_, 0, sizeof(self->ip_));
        xEventGroupClearBits(self->event_group_, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(self->event_group_, WIFI_FAIL_BIT);
        break;
    }

    /* AP启动成功 */
    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "AP started");
        self->state_ = WIFI_STATE_AP_ACTIVE;
        break;

    /* AP停止 */
    case WIFI_EVENT_AP_STOP:
        ESP_LOGI(TAG, "AP stopped");
        if (self->state_ == WIFI_STATE_AP_ACTIVE) {
            self->state_ = WIFI_STATE_DISCONNECTED;
        }
        break;

    /* 有新STA连接到AP */
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t* event = static_cast<wifi_event_ap_staconnected_t*>(event_data);
        ESP_LOGI(TAG, "STA joined AP, MAC:%02x:%02x:%02x:%02x:%02x:%02x AID:%d", event->mac[0], event->mac[1],
                 event->mac[2], event->mac[3], event->mac[4], event->mac[5], event->aid);
        break;
    }

    /* 有STA断开与AP的连接 */
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t* event = static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
        ESP_LOGI(TAG, "STA left AP, MAC:%02x:%02x:%02x:%02x:%02x:%02x AID:%d", event->mac[0], event->mac[1],
                 event->mac[2], event->mac[3], event->mac[4], event->mac[5], event->aid);
        break;
    }

    /* 扫描完成 */
    case WIFI_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "Scan done");
        xEventGroupSetBits(self->event_group_, WIFI_SCAN_DONE_BIT);
        break;

    default:
        break;
    }
}

/**
 * @brief IP事件回调
 *
 * 处理IP地址获取/丢失事件，更新内部IP状态和连接标志。
 */
void WiFiManager::ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    WiFiManager* self = static_cast<WiFiManager*>(arg);

    if (event_base != IP_EVENT) {
        return;
    }

    switch (event_id) {
    /* DHCP成功获取IP地址 */
    case IP_EVENT_STA_GOT_IP: {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        self->ip_.addr           = event->ip_info.ip.addr;
        self->state_             = WIFI_STATE_STA_CONNECTED;
        xEventGroupClearBits(self->event_group_, WIFI_FAIL_BIT);
        xEventGroupSetBits(self->event_group_, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        break;
    }

    /* IP地址丢失 */
    case IP_EVENT_STA_LOST_IP:
        self->state_ = WIFI_STATE_DISCONNECTED;
        memset(&self->ip_, 0, sizeof(self->ip_));
        ESP_LOGW(TAG, "Lost IP");
        break;

    default:
        break;
    }
}

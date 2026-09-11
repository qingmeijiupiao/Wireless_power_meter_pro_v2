/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: WiFi/Web/ESP-NOW 射频策略服务，封装 STA、AP 配网、ESPNOW_ONLY 和 NVS 配置
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-05-28
 */
#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <cstddef>

#include "esp_err.h"
#include "wifi_manager.h"

namespace WifiService {

/** AP 配网模式默认 IPv4 地址，修改配网网段时只需要调整这里 */
constexpr uint8_t AP_IP_OCTET1 = 192;
constexpr uint8_t AP_IP_OCTET2 = 168;
constexpr uint8_t AP_IP_OCTET3 = 4;
constexpr uint8_t AP_IP_OCTET4 = 1;

/**
 * @brief WiFiService 当前工作模式
 */
enum class Mode : uint8_t {
    OFF = 0,      /**< WiFi 射频尚未启动，仅用于初始化前或完整反初始化状态 */
    ESPNOW_ONLY,  /**< 仅启动 STA 射频，供 ESP-NOW 通信使用 */
    STA,          /**< STA 模式，已连接到外部路由器并通过路由器 IP 提供 Web 服务 */
    AP_PROVISION, /**< AP 配网模式，设备启动热点并开启 DNS 劫持 */
};

/**
 * @brief WiFiService 持久化配置快照
 *
 * @note password 会完整返回给内部调用方，Web API 不应直接暴露该字段。
 */
struct Config {
    char ssid[WIFI_SSID_MAX_LEN + 1]         = {};    /**< 已保存的 STA SSID，空字符串表示未配置。 */
    char password[WIFI_PASSWORD_MAX_LEN + 1] = {};    /**< 已保存的 STA 密码，开放网络可为空。 */
    bool web_enabled_on_boot                 = false; /**< 启动时是否自动启用 WiFi/Web 相关功能。 */
};

constexpr size_t WIFI_SCAN_MAX_RESULTS = 12;

struct ScanResult {
    char             ssid[WIFI_SSID_MAX_LEN + 1] = {};
    int8_t           rssi                        = 0;
    uint8_t          channel                     = 0;
    wifi_auth_mode_t authmode                    = WIFI_AUTH_OPEN;
};

/**
 * @brief 初始化 WiFiService
 *
 * 初始化 NVS、WiFiManager、EspNowLink 和 EspNowService，并生成默认配网热点名。
 *
 * @return ESP_OK 成功，其他值表示 WiFiManager 初始化失败
 */
esp_err_t init();

/**
 * @brief 按 NVS 配置启动默认网络模式
 *
 * 流程：
 * 1. 若 web_boot 为 0，则进入 ESPNOW_ONLY。
 * 2. 若已保存 SSID，则尝试 STA 连接。
 * 3. STA 连接失败或未保存 SSID 时，切换到 AP 配网模式。
 *
 * @return ESP_OK 成功进入某种可用模式，其他值表示底层 WiFi/AP 启动失败
 */
esp_err_t start_default(const char* source);

/** @brief 启动仅 ESP-NOW 使用的 STA 射频模式 */
esp_err_t start_espnow_only(const char* source);

/**
 * @brief 关闭 IP 网络和 Web 所需射频模式，保留 ESP-NOW
 *
 * 停止 DNS 劫持、关闭 Captive Portal，并切换到 ESPNOW_ONLY。
 *
 * @return ESP_OK 成功，其他值来自 ESPNOW_ONLY 启动流程
 */
esp_err_t stop(const char* source);

/**
 * @brief 连接到指定 STA WiFi
 *
 * @param ssid WiFi SSID，不能为空
 * @param password WiFi 密码，开放网络传空字符串
 * @param save true 表示连接成功后写入 NVS，false 表示仅本次连接
 * @return ESP_OK 连接成功，ESP_ERR_INVALID_ARG 参数非法，ESP_ERR_TIMEOUT 等表示连接失败
 */
esp_err_t connect_sta(const char* ssid, const char* password, bool save, const char* source);

/**
 * @brief 启动 AP 配网模式
 *
 * 使用 `WPM-PRO-V2-XXXXXX` 作为开放热点名，启动 DNS 劫持并开启 WebServer Captive Portal 回落。
 *
 * @return ESP_OK 成功，其他值来自 WiFiManager 或 DNSServer
 */
esp_err_t start_provision_ap(const char* source);

/**
 * @brief 扫描附近 WiFi AP
 *
 * AP 配网模式下使用 APSTA，扫描期间配网热点仍保持运行，但无线侧会有短暂延迟。
 *
 * @param results 输出缓冲区
 * @param max_results 输出缓冲区容量
 * @param out_count 实际写入数量
 * @return ESP_OK 成功，ESP_ERR_INVALID_ARG 参数非法，其他值来自底层 WiFi 扫描
 */
esp_err_t scan_ap_list(ScanResult* results, size_t max_results, size_t* out_count);

/**
 * @brief 设置启动时是否默认启用 WiFi/Web 功能
 *
 * @param enabled true 启用，false 禁用
 * @return ESP_OK 当前实现固定返回成功
 */
esp_err_t set_web_enabled_on_boot(bool enabled, const char* source);

/**
 * @brief 清除已保存的 STA SSID 和密码
 *
 * @return ESP_OK 当前实现固定返回成功
 */
esp_err_t clear_saved_sta(const char* source);

/** @brief 查询 WiFiService 是否已初始化 */
bool is_initialized();

/** @brief 查询启动时是否默认启用 WiFi/Web 功能 */
bool is_web_enabled_on_boot();

/** @brief 查询 NVS 中是否已保存 STA SSID */
bool has_saved_sta();

/** @brief 查询当前是否处于 AP 配网模式 */
bool is_provisioning();

/** @brief 获取当前 WiFiService 模式 */
Mode get_mode();

/** @brief 获取当前 NVS 配置快照 */
Config get_config();

/** @brief 获取当前配网 AP 的 SSID */
const char* get_ap_ssid();

/** @brief 获取最近一次 WiFiService 操作错误描述 */
const char* get_last_error();

/**
 * @brief 获取当前对外访问 IP
 *
 * AP 配网模式返回 `AP_IP_OCTET*` 定义的地址；STA 模式返回 WiFiManager 当前 IP。
 */
IP_t get_ip();

/** @brief 获取底层 WiFiManager 状态 */
wifi_state_t get_wifi_state();

/** @brief 获取当前 STA 连接的 RSSI，未连接时返回 0 */
int8_t get_rssi();

/** @brief 获取当前 STA 信号强度百分比，范围 0-100 */
uint8_t get_signal_percent();

/**
 * @brief 获取当前 WiFi 主信道
 * @param channel 输出主信道
 * @return ESP_OK 成功，其他值来自底层 WiFiManager
 */
esp_err_t get_channel(uint8_t* channel);

/** @brief 设置并持久化 ESP-NOW-only 模式使用的信道 */
esp_err_t set_espnow_channel(uint8_t channel);

} // namespace WifiService

#endif

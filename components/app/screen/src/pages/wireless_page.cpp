/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 无线状态页面实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-24
 */
#include "pages/wireless_page.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>

#include "blackbox.h"
#include "diagnostic_log.h"
#include "DENGB16.h"
#include "DENGB20.h"
#include "current_calibration.h"
#include "energy_meter.h"
#include "espnow_link.h"
#include "espnow_service.h"
#include "esp_log.h"
#include "blackbox_service.h"
#include "can_callback.h"
#include "can_resistor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "HXC_NVS.h"
#include "st7789.h"
#include "widgets/ui_chrome.h"
#include "ota_service.h"
#include "wifi_manager.h"
#include "wifi_service.h"

namespace SCREEN {
namespace {

constexpr char TAG[] = "ScreenPages";

/** @brief 将 WiFi 服务模式转换为页面短文本。 */
const char* wifi_mode_text(WifiService::Mode mode) {
    switch (mode) {
    case WifiService::Mode::OFF:
        return "OFF";
    case WifiService::Mode::ESPNOW_ONLY:
        return "NOW";
    case WifiService::Mode::STA:
        return "STA";
    case WifiService::Mode::AP_PROVISION:
        return "AP";
    default:
        return "UNK";
    }
}

} // namespace

PageId WirelessPage::id() const {
    return PageId::Wireless;
}

/** @brief 返回无线页标题。 */
const char *WirelessPage::title() const { return "Wireless"; }

/** @brief 返回无线页刷新周期。 */
uint32_t WirelessPage::refresh_interval_ms() const {
    return 500;
}

/**
 * @brief 处理无线页按键，侧键长按时进入 AP 配网模式。
 * @param button 按键 ID。
 * @param event 按键事件。
 * @return true 表示事件已处理。
 */
bool WirelessPage::handle_button(ButtonId button, ButtonEvent event) {
    if (button != ButtonId::Side || event != ButtonEvent::LONG_PRESS) {
        return false;
    }

    last_result_ = WifiService::start_provision_ap(TAG);
    return true;
}

/**
 * @brief 按当前网络模式绘制无线状态页。
 * @param mode 页面渲染模式，无线页始终执行整屏重绘。
 */
void WirelessPage::render(RenderMode mode) {
    (void)mode;
    ST7789::fill_screen(ST7789::BLACK);
    char value[64];
    const auto wifi_mode = WifiService::get_mode();
    const bool              provisioning = WifiService::is_provisioning();
    const char*             mode_text    = provisioning ? "AP" : wifi_mode_text(wifi_mode);
    auto mode_color = wifi_mode == WifiService::Mode::STA ? UI::CURRENT : UI::CYAN;
    if (last_result_ != ESP_OK && wifi_mode == WifiService::Mode::OFF) {
        mode_text  = "ERR";
        mode_color = UI::VOLTAGE;
    }
    UI::badge(6, 6, 45, 22, mode_text, mode_color, UI::PANEL);

    EspNowService::RemoteSwitchStatus remote = {};
    if (EspNowService::get_remote_switch_status(remote) && remote.battery_valid) {
        snprintf(value, sizeof(value), "%" PRIu32 "%%", static_cast<uint32_t>(remote.battery_percent));
        ST7789::draw_round_rect(184, 7, 46, 20, 3, 1, ST7789::WHITE, ST7789::BLACK);
        ST7789::fill_rect(231, 13, 3, 8, ST7789::WHITE);
        UI::text(187, 7, 40, 20, value, remote.battery_percent <= 20 ? UI::VOLTAGE : UI::CURRENT, ST7789::BLACK,
                 DENGB16, UI::Align::Center);
    }

    const auto cfg = WifiService::get_config();
    const auto ip = WifiService::get_ip();
        uint8_t             channel           = 0;
        const bool          channel_available = WifiService::get_channel(&channel) == ESP_OK;
        const uint8_t       signal = wifi_mode == WifiService::Mode::STA ? WifiService::get_signal_percent() : 0;
        if (provisioning) {
        snprintf(value, sizeof(value), "%s", WifiService::get_ap_ssid());
        } else if (wifi_mode == WifiService::Mode::STA) {
        snprintf(value, sizeof(value), "%.32s", cfg.ssid[0] == '\0' ? "Connected" : cfg.ssid);
        } else if (wifi_mode == WifiService::Mode::ESPNOW_ONLY) {
            snprintf(value, sizeof(value), "ESP-NOW only");
    } else {
        snprintf(value, sizeof(value), "%s", last_result_ != ESP_OK ? "ERR" : "OFF");
    }
    ST7789::fill_round_rect(6, 36, 228, 25, 5, UI::PANEL, ST7789::BLACK);
    UI::text(12, 36, 34, 25, "SSID", UI::MUTED, UI::PANEL, DENGB16);
    UI::text(51, 36, 177, 25, value, wifi_mode == WifiService::Mode::OFF ? UI::MUTED : ST7789::WHITE, UI::PANEL,
             DENGB16);

    const bool connected =
            wifi_mode == WifiService::Mode::STA && WifiService::get_wifi_state() == WIFI_STATE_STA_CONNECTED;
    const uint8_t bars = provisioning ? 4 : (connected ? std::min<uint8_t>(4, (signal + 24) / 25) : 0);
    for (uint8_t i = 0; i < 4; ++i) {
        const uint16_t height = 20 + i * 12;
        ST7789::fill_round_rect(9 + i * 13, 125 - height, 9, height, 4, i < bars ? ST7789::WHITE : UI::PANEL,
                                    ST7789::BLACK);
    }
    snprintf(value, sizeof(value), "IP:%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32, static_cast<uint32_t>(ip.octet1),
             static_cast<uint32_t>(ip.octet2), static_cast<uint32_t>(ip.octet3), static_cast<uint32_t>(ip.octet4));
    UI::badge(68, 73, 166, 24, value, provisioning ? UI::CURRENT : UI::CYAN, UI::PANEL);
        if (wifi_mode == WifiService::Mode::STA && channel_available) {
        snprintf(value, sizeof(value), "SIG CH%" PRIu32 " %" PRIu32 "%%", static_cast<uint32_t>(channel),
                     static_cast<uint32_t>(signal));
        } else if (provisioning) {
            snprintf(value, sizeof(value), "AP mode");
        } else if (wifi_mode == WifiService::Mode::ESPNOW_ONLY && channel_available) {
            snprintf(value, sizeof(value), "CH%" PRIu32 " NOW", static_cast<uint32_t>(channel));
    } else if (last_result_ != ESP_OK) {
        snprintf(value, sizeof(value), "%s", esp_err_to_name(last_result_));
    } else {
            snprintf(value, sizeof(value), "Hold AP");
    }
    UI::badge(68, 107, 166, 22, value, UI::MUTED, UI::PANEL);
}

/** @brief 返回设置页 ID。 */

} // namespace SCREEN

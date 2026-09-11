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
#include "DENGB12.h"
#include "DENGB16.h"
#include "DENGB20.h"
#include "current_calibration.h"
#include "energy_meter.h"
#include "espnow_link.h"
#include "espnow_service.h"
#include "esp_log.h"
#include "ErrorRectangle.h"
#include "WarningRectangle.h"
#include "blackbox_service.h"
#include "can_callback.h"
#include "can_resistor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "HXC_NVS.h"
#include "meter_a_logo.h"
#include "meter_circle_green.h"
#include "meter_circle_red.h"
#include "meter_v_logo.h"
#include "meter_w_logo.h"
#include "settings_logo.h"
#include "st7789.h"
#include "ota_service.h"
#include "ui_close.h"
#include "ui_open.h"
#include "ui_static.h"
#include "wifi_manager.h"
#include "wifi_service.h"
#include "ah_logo.h"
#include "wh_logo.h"

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
const char* WirelessPage::title() const {
    return "Wireless";
}

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

    char                    value[24];
    const WifiService::Mode wifi_mode    = WifiService::get_mode();
    const bool              provisioning = WifiService::is_provisioning();
    const char*             mode_text    = provisioning ? "AP" : wifi_mode_text(wifi_mode);
    ST7789::color_t         mode_color =
        wifi_mode == WifiService::Mode::STA ? ST7789::color_t(0x1ef851) : ST7789::color_t(0x2FC9EC);
    if (last_result_ != ESP_OK && wifi_mode == WifiService::Mode::OFF) {
        mode_text  = "ERR";
        mode_color = ST7789::color_t(0xef2a2a);
    }

    auto draw_status_pill = [](uint16_t x, uint16_t y, const char* text, ST7789::color_t color) {
        constexpr uint16_t    pill_w     = 36;
        constexpr uint16_t    pill_h     = 17;
        const ST7789::color_t background = ST7789::color_t(0x202020);
        ST7789::fill_round_rect(x, y, pill_w, pill_h, 6, background, ST7789::BLACK);
        ST7789::draw_string(x + 3, 2, text, color, background, DENGB16);
    };

    auto draw_remote_battery = []() {
        EspNowService::RemoteSwitchStatus status = {};
        if (!EspNowService::get_remote_switch_status(status) || !status.battery_valid) {
            return;
        }

        constexpr uint16_t body_x  = 126;
        constexpr uint16_t body_y  = 1;
        constexpr uint16_t body_w  = 28;
        constexpr uint16_t body_h  = 16;
        char               text[5] = {};
        if (status.battery_percent == 100) {
            snprintf(text, sizeof(text), "100");
        } else {
            snprintf(text, sizeof(text), "%" PRIu32 "%%", static_cast<uint32_t>(status.battery_percent));
        }

        uint16_t text_w = 0;
        for (const char* cursor = text; *cursor != '\0'; ++cursor) {
            text_w += DENGB12.width_table[*cursor - ' '];
        }
        const ST7789::color_t text_color =
            status.battery_percent <= 20 ? ST7789::color_t(0xef2a2a) : ST7789::color_t(0x1ef851);

        ST7789::draw_round_rect(body_x, body_y, body_w, body_h, 3, 1, ST7789::WHITE, ST7789::BLACK);
        ST7789::fill_rect(body_x + body_w, body_y + 4, 4, 8, ST7789::WHITE);
        ST7789::draw_string(body_x + (body_w - text_w) / 2, body_y + 4, text, text_color, ST7789::BLACK, DENGB12);
    };

    auto draw_signal_logo = [&]() {
        constexpr uint16_t    bar_x0         = 7;
        constexpr uint16_t    bar_bottom     = 76;
        constexpr uint16_t    bar_w          = 7;
        constexpr uint16_t    bar_gap        = 4;
        constexpr uint16_t    bar_radius     = 3;
        constexpr uint16_t    bar_heights[4] = {14, 23, 32, 41};
        const ST7789::color_t inactive       = ST7789::color_t(0x303030);
        const bool            sta_connected =
            wifi_mode == WifiService::Mode::STA && WifiService::get_wifi_state() == WIFI_STATE_STA_CONNECTED;
        const uint8_t signal      = sta_connected ? WifiService::get_signal_percent() : 0;
        uint8_t       active_bars = provisioning ? 4 : 0;
        if (!provisioning && signal > 0) {
            active_bars = static_cast<uint8_t>((signal + 24) / 25);
            if (active_bars > 4) {
                active_bars = 4;
            }
        }

        for (uint8_t i = 0; i < 4; i++) {
            const uint16_t x = bar_x0 + i * (bar_w + bar_gap);
            const uint16_t h = bar_heights[i];
            const uint16_t y = bar_bottom - h;
            ST7789::fill_round_rect(x, y, bar_w, h, bar_radius, i < active_bars ? ST7789::WHITE : inactive,
                                    ST7789::BLACK);
        }
    };

    auto draw_info_row = [](uint16_t x, uint16_t y, uint16_t w, const char* label, const char* text,
                            ST7789::color_t text_color) {
        constexpr uint16_t    row_h       = 14;
        constexpr uint16_t    row_radius  = 5;
        const ST7789::color_t background  = ST7789::color_t(0x202020);
        const ST7789::color_t label_color = ST7789::color_t(0xB5B5B5);
        ST7789::fill_round_rect(x, y, w, row_h, row_radius, background, ST7789::BLACK);
        ST7789::draw_string(x + 4, y + 3, label, label_color, background, DENGB12);
        ST7789::draw_string(x + 33, y + 3, text, text_color, background, DENGB12);
    };

    auto draw_text_row = [](uint16_t x, uint16_t y, uint16_t w, const char* text, ST7789::color_t text_color) {
        constexpr uint16_t    row_h      = 14;
        constexpr uint16_t    row_radius = 5;
        const ST7789::color_t background = ST7789::color_t(0x202020);
        ST7789::fill_round_rect(x, y, w, row_h, row_radius, background, ST7789::BLACK);
        ST7789::draw_string(x + 4, y + 3, text, text_color, background, DENGB12);
    };

    auto draw_details = [&]() {
        WifiService::Config cfg               = WifiService::get_config();
        IP_t                ip                = WifiService::get_ip();
        uint8_t             channel           = 0;
        const bool          channel_available = WifiService::get_channel(&channel) == ESP_OK;
        const uint8_t       signal = wifi_mode == WifiService::Mode::STA ? WifiService::get_signal_percent() : 0;

        if (provisioning) {
            snprintf(value, sizeof(value), "%.18s", WifiService::get_ap_ssid());
        } else if (wifi_mode == WifiService::Mode::STA) {
            snprintf(value, sizeof(value), "%.18s", cfg.ssid[0] == '\0' ? "Connected" : cfg.ssid);
        } else if (wifi_mode == WifiService::Mode::ESPNOW_ONLY) {
            snprintf(value, sizeof(value), "ESP-NOW only");
        } else if (last_result_ != ESP_OK) {
            snprintf(value, sizeof(value), "ERR");
        } else {
            snprintf(value, sizeof(value), "OFF");
        }
        draw_info_row(2, 19, 156, "SSID", value,
                      wifi_mode == WifiService::Mode::OFF ? ST7789::color_t(0xB5B5B5) : ST7789::WHITE);

        snprintf(value, sizeof(value), "IP:%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 static_cast<uint32_t>(ip.octet1), static_cast<uint32_t>(ip.octet2),
                 static_cast<uint32_t>(ip.octet3), static_cast<uint32_t>(ip.octet4));
        draw_text_row(54, 42, 104, value, provisioning ? ST7789::color_t(0x1ef851) : ST7789::color_t(0x2FC9EC));

        if (wifi_mode == WifiService::Mode::STA && channel_available) {
            snprintf(value, sizeof(value), "CH%" PRIu32 " %" PRIu32 "%%", static_cast<uint32_t>(channel),
                     static_cast<uint32_t>(signal));
        } else if (provisioning) {
            snprintf(value, sizeof(value), "AP mode");
        } else if (wifi_mode == WifiService::Mode::ESPNOW_ONLY && channel_available) {
            snprintf(value, sizeof(value), "CH%" PRIu32 " NOW", static_cast<uint32_t>(channel));
        } else if (last_result_ != ESP_OK) {
            snprintf(value, sizeof(value), "%.12s", esp_err_to_name(last_result_));
        } else {
            snprintf(value, sizeof(value), "Hold AP");
        }
        draw_info_row(54, 60, 104, "SIG", value, ST7789::color_t(0xB5B5B5));
    };

    draw_status_pill(2, 0, mode_text, mode_color);
    draw_remote_battery();
    draw_signal_logo();
    draw_details();
}

/** @brief 返回设置页 ID。 */

} // namespace SCREEN

/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 设置页面菜单、业务动作与弹窗实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-24
 */
#include "pages/settings/settings_page.h"

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

constexpr uint32_t CAN_BAUDRATES[] = {
    1_Mbps,
    500_Kbps,
    250_Kbps,
    125_Kbps,
};

constexpr uint32_t BLACKBOX_SNAPSHOT_INTERVALS_S[] = {
    0, 1, 5, 10, 30, 60,
};

} // namespace

PageId SettingsPage::id() const {
    return PageId::Settings;
}

/** @brief 返回设置页标题。 */
const char *SettingsPage::title() const { return "Settings"; }

/** @brief 返回设置页刷新周期。 */
uint32_t SettingsPage::refresh_interval_ms() const {
    return 200;
}

/** @brief 声明设置页支持菜单编辑模式。 */
bool SettingsPage::supports_edit_mode() const {
    return true;
}

/** @brief 返回设置页菜单是否处于激活状态。 */
bool SettingsPage::is_overlay_active() const {
    return mode_ != Mode::View;
}

/** @brief 进入设置页时刷新持久化显示配置。 */
void SettingsPage::on_enter() {
    load_config();
}

/** @brief 进入设置菜单。 */
void SettingsPage::on_edit_enter() {
    mode_ = Mode::Menu;
}

/** @brief 退出设置菜单。 */
void SettingsPage::on_edit_exit() {
    mode_ = Mode::View;
}

/**
 * @brief 处理设置菜单内的选择、修改和退出操作。
 * @param button 按键 ID。
 * @param event 按键事件。
 * @return true 表示事件已由设置页消费。
 */
bool SettingsPage::handle_button(ButtonId button, ButtonEvent event) {
    if (mode_ == Mode::View) {
        return false;
    }

    const bool page_short = event == ButtonEvent::SHORT_PRESS &&
                            (button == ButtonId::Side || button == ButtonId::Previous);

    if (mode_ == Mode::Dialog) {
        if (selected_ == FirmwareUpdate) {
            const OtaService::Status ota = OtaService::get_status();
            // 上下翻页键都可取消确认，主按键仍负责确认/重试。
            if (page_short) {
                update_confirm_ = false;
                mode_           = Mode::Menu;
                return true;
            }
            // 主按键只有按下边沿：第一次按下进入确认态，再次按下确认升级。
            if (!update_confirm_ && button == ButtonId::Main && event == ButtonEvent::PRESS &&
                ota.state == OtaService::State::UPDATE_AVAILABLE) {
                update_confirm_ = true;
                return true;
            }
            if (update_confirm_ && button == ButtonId::Main && event == ButtonEvent::PRESS) {
                const esp_err_t err = OtaService::request_upgrade();
                if (err == ESP_OK) {
                    update_confirm_ = false;
                }
                return true;
            }
            if (button == ButtonId::Main && event == ButtonEvent::PRESS &&
                (ota.state == OtaService::State::FAILED || ota.state == OtaService::State::UP_TO_DATE)) {
                OtaService::request_check();
                return true;
            }
            return true;
        }

        // 详情信息弹窗：上下翻页键和主按键都可退出。
        if (page_short || (button == ButtonId::Main && event == ButtonEvent::PRESS)) {
            mode_ = Mode::Menu;
            return true;
        }

        if (button == ButtonId::Side && event == ButtonEvent::LONG_PRESS) {
            mode_ = Mode::View;
            return true;
        }

        return true;
    }

    // 菜单中上翻页键回退一项，下翻页键前进一项，避免另一颗翻页键落到全局翻页。
    if (button == ButtonId::Side && event == ButtonEvent::SHORT_PRESS) {
        selected_ = static_cast<uint8_t>((selected_ + 1) % ITEM_COUNT);
        return true;
    }

    if (button == ButtonId::Previous && event == ButtonEvent::SHORT_PRESS) {
        selected_ = static_cast<uint8_t>((selected_ + ITEM_COUNT - 1) % ITEM_COUNT);
        return true;
    }

    if (button == ButtonId::Main && event == ButtonEvent::PRESS) {
        activate_selected_item();
        return true;
    }

    if (button == ButtonId::Side && event == ButtonEvent::LONG_PRESS) {
        mode_ = Mode::View;
        return true;
    }

    return true;
}

/**
 * @brief 绘制设置页图标和当前可见菜单项。
 * @param mode 页面渲染模式，设置页始终执行整屏重绘。
 */
void SettingsPage::render(RenderMode mode) {
    (void)mode;
    ST7789::fill_screen(ST7789::BLACK);
    // Draw the original settings sliders without adding a bitmap resource.
    constexpr uint16_t knob_y[] = {52, 72, 59};
    for (uint8_t i = 0; i < 3; ++i) {
        ST7789::fill_round_rect(10 + i * 14, 43, 3, 47, 1, UI::CYAN, ST7789::BLACK);
        ST7789::fill_round_rect(6 + i * 14, knob_y[i], 11, 8, 3, UI::CYAN, ST7789::BLACK);
    }
    for (uint8_t row = 0; row < VISIBLE_ROWS; ++row) {
        const uint8_t item = (selected_ + ITEM_COUNT + row - 1) % ITEM_COUNT;
        const uint16_t y = 8 + row * 40;
        const bool selected = row == 1 && mode_ != Mode::View;
        const auto background = selected ? UI::YELLOW : UI::PANEL;
        const auto foreground = selected ? ST7789::BLACK : ST7789::WHITE;
        ST7789::fill_round_rect(60, y, 174, 33, 5, background, ST7789::BLACK);
        const char* value = item_value(item);
        const bool detail = item_type(item) == ItemType::Detail;
        const uint16_t value_w = UI::text_width(value, DENGB16);
        const uint16_t name_w = detail ? 132 : 158 - value_w - (value_w ? 6 : 0);
        const char* name = item_name(item);
        if (UI::text_width(name, DENGB16) > name_w) {
            switch (item) {
            case WebBoot: name = "Web server"; break;
            case BlackboxSnapshot: name = "Snapshot"; break;
            case EspNowPair: name = "Pair devices"; break;
            case EspNowInfo: name = "ESP-NOW info"; break;
            case CanBaudrate: name = "CAN rate"; break;
            case CanTerm: name = "Termination"; break;
            case FirmwareInfo: name = "Firmware info"; break;
            case FirmwareUpdate: name = "Update"; break;
            case BlackboxInfo: name = "Blackbox info"; break;
            default: break;
            }
        }
        UI::text(68, y, name_w, 33, name, foreground, background);
        if (detail) {
            ST7789::draw_round_rect(208, y + 7, 18, 18, 9, 1, foreground, background);
            UI::text(209, y + 7, 16, 18, "i", foreground, background, DENGB16, UI::Align::Center);
        } else {
            UI::text(226 - value_w, y, value_w, 33, value, foreground, background, DENGB16, UI::Align::Right);
        }
    }
    if (mode_ == Mode::Dialog)
        draw_dialog_overlay();
}

/** @brief 从 NVS 加载设置页使用的显示配置。 */
void SettingsPage::load_config() {
    rotation_180_    = ui_config_get_rotation_180();
    backlight_level_ = ui_config_get_backlight_level();
}

/**
 * @brief 返回设置项名称。
 * @param item 设置项索引。
 * @return 设置项显示名称。
 */
const char* SettingsPage::item_name(uint8_t item) const {
    switch (item) {
    case Rotate180:
        return "Rotation";
    case Backlight:
        return "Brightness";
    case WebBoot:
        return "Web server at boot";
    case ProtectBypass:
        return "Protection";
    case BlackboxSnapshot:
        return "Blackbox snapshot";
    case EspNowPair:
        return "ESP-NOW pairing";
    case EspNowInfo:
        return "ESP-NOW information";
    case CanBaudrate:
        return "CAN baud rate";
    case CanTerm:
        return "CAN termination";
    case FirmwareInfo:
        return "Firmware information";
    case FirmwareUpdate:
        return "Firmware update";
    case BlackboxInfo:
        return "Blackbox information";
    case CalibrationInfo:
        return "Calibration";
    default:
        return "";
    }
}

/**
 * @brief 返回设置项当前值的显示文本。
 * @param item 设置项索引。
 * @return 设置项值文本。
 */
const char* SettingsPage::item_value(uint8_t item) {
    switch (item) {
    case Rotate180:
        return rotation_180_ ? "180" : "0";
    case Backlight:
        snprintf(value_buf_, sizeof(value_buf_), "%" PRIu32 "/5", static_cast<uint32_t>(backlight_level_));
        return value_buf_;
    case WebBoot:
        return WifiService::is_web_enabled_on_boot() ? "ON" : "OFF";
    case ProtectBypass:
        return protect_is_bypassed() ? "OFF" : "ON";
    case BlackboxSnapshot: {
        switch (BlackboxService::get_snapshot_interval_s()) {
        case 0:
            return "OFF";
        case 1:
            return "1s";
        case 5:
            return "5s";
        case 10:
            return "10s";
        case 30:
            return "30s";
        case 60:
            return "60s";
        default:
            return "Other";
        }
    }
    case EspNowPair:
        return EspNowLink::is_pairing() ? "Pairing" : "";
    case EspNowInfo:
        snprintf(value_buf_, sizeof(value_buf_), "%" PRIu32 "/3",
                 static_cast<uint32_t>(EspNowLink::get_saved_peer_count()));
        return value_buf_;
    case CanBaudrate:
        switch (CanCallback::CAN_BAUDRATE.read()) {
        case 1_Mbps:
            return "1 Mbps";
        case 500_Kbps:
            return "500 kbps";
        case 250_Kbps:
            return "250 kbps";
        case 125_Kbps:
            return "125 kbps";
        default:
            return "Other";
        }
    case CanTerm:
        return CanResistor::instance().get() ? "ON" : "OFF";
    case FirmwareInfo:
        return "";
    case FirmwareUpdate: {
        const OtaService::State state = OtaService::get_status().state;
        if (state == OtaService::State::UPDATE_AVAILABLE)
            return "Available";
        if (state == OtaService::State::CHECKING || state == OtaService::State::DOWNLOADING ||
            state == OtaService::State::VERIFYING)
            return "Busy";
        if (state == OtaService::State::FAILED)
            return "Error";
        return "";
    }
    case BlackboxInfo:
    case CalibrationInfo:
        return "";
    default:
        return "";
    }
}

/** @brief 获取设置项交互类型。 */
SettingsPage::ItemType SettingsPage::item_type(uint8_t item) const {
    switch (item) {
    case EspNowPair:
    case FirmwareUpdate:
        return ItemType::Action;
    case EspNowInfo:
    case FirmwareInfo:
    case BlackboxInfo:
    case CalibrationInfo:
        return ItemType::Detail;
    default:
        return ItemType::Adjustable;
    }
}

/** @brief 激活当前选中项。 */
void SettingsPage::activate_selected_item() {
    switch (item_type(selected_)) {
    case ItemType::Adjustable:
        adjust_selected_item();
        break;
    case ItemType::Detail:
        mode_ = Mode::Dialog;
        break;
    case ItemType::Action:
        if (run_action_item(selected_)) {
            mode_ = Mode::Dialog;
        }
        break;
    }
}

/** @brief 运行动作类设置项。 */
bool SettingsPage::run_action_item(uint8_t item) {
    if (item == FirmwareUpdate) {
        update_confirm_              = false;
        const OtaService::Status ota = OtaService::get_status();
        if (ota.state != OtaService::State::UPDATE_AVAILABLE && ota.state != OtaService::State::CHECKING &&
            ota.state != OtaService::State::DOWNLOADING && ota.state != OtaService::State::VERIFYING &&
            ota.state != OtaService::State::RESTARTING) {
            OtaService::request_check();
        }
        return true;
    }
    if (item != EspNowPair) {
        return false;
    }

    esp_err_t ret = ESP_OK;
    if (EspNowLink::is_pairing()) {
        EspNowLink::leave_pairing_mode();
        DEVICE_EVENT_I(TAG, "espnow: pairing source=screen action=stop result=ok");
    } else {
        ret = EspNowLink::enter_pairing_mode(0);
        if (ret == ESP_OK) {
            DEVICE_EVENT_I(TAG, "espnow: pairing source=screen action=start unlimited=1 result=ok");
        } else {
            ESP_LOGW(TAG, "espnow: pairing source=screen target=active result=%s", esp_err_to_name(ret));
        }
    }
    return true;
}

/** @brief 刷新当前弹窗内容。 */
void SettingsPage::build_dialog_content() {
    const uint8_t item = selected_;

    for (auto& line : detail_lines_) {
        line[0] = '\0';
    }

    if (item == EspNowPair) {
        snprintf(detail_lines_[0], sizeof(detail_lines_[0]), "State %s",
                 EspNowLink::is_pairing() ? "PAIRING" : "STOPPED");
        snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Paired %" PRIu32 "/3",
                 static_cast<uint32_t>(EspNowLink::get_saved_peer_count()));
        snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "No time limit");
        snprintf(detail_lines_[3], sizeof(detail_lines_[3]), "Exit after success");
    } else if (item == EspNowInfo) {
        const size_t count = EspNowLink::get_saved_peer_count();
        snprintf(detail_lines_[0], sizeof(detail_lines_[0]), "Paired %" PRIu32 "/3", static_cast<uint32_t>(count));
        for (size_t i = 0; i < 3; ++i) {
            EspNowLink::SavedPeer peer = {};
            if (i < count && EspNowLink::get_saved_peer(i, &peer) == ESP_OK) {
                snprintf(detail_lines_[i + 1], sizeof(detail_lines_[i + 1]), "%02X:%02X:%02X:%02X:%02X:%02X",
                         peer.address.bytes[0], peer.address.bytes[1], peer.address.bytes[2], peer.address.bytes[3],
                         peer.address.bytes[4], peer.address.bytes[5]);
            } else {
                snprintf(detail_lines_[i + 1], sizeof(detail_lines_[i + 1]), "--");
            }
        }
    } else if (item == FirmwareInfo) {
        const MAC_t mac = WiFiManager::instance().get_mac(WIFI_IF_STA);
        snprintf(detail_lines_[0], sizeof(detail_lines_[0]), "Version %" PRIu32 ".%" PRIu32 ".%" PRIu32 " %s",
                 static_cast<uint32_t>(VERSION_MAJOR), static_cast<uint32_t>(VERSION_MINOR),
                 static_cast<uint32_t>(VERSION_PATCH), VERSION_PATCH == 99 ? "Test" : "Release");
        snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Build Time");
        snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "%.16s", BUILD_TIME);
        snprintf(detail_lines_[3], sizeof(detail_lines_[3]), "MAC %02X:%02X:%02X:%02X:%02X:%02X", mac.octet1,
                 mac.octet2, mac.octet3, mac.octet4, mac.octet5, mac.octet6);
    } else if (item == FirmwareUpdate) {
        const OtaService::Status ota = OtaService::get_status();
        if (update_confirm_) {
            snprintf(detail_lines_[0], sizeof(detail_lines_[0]), "Upgrade to %.15s", ota.latest_version);
            snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Hold MAIN confirm");
            snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "SIDE cancel");
            snprintf(detail_lines_[3], sizeof(detail_lines_[3]), "Automatic restart");
        } else {
            snprintf(detail_lines_[0], sizeof(detail_lines_[0]), "State %s", OtaService::state_to_string(ota.state));
            if (ota.state == OtaService::State::UPDATE_AVAILABLE) {
                snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "%.11s -> %.11s", ota.current_version,
                         ota.latest_version);
                snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "MAIN to confirm");
            } else if (ota.state == OtaService::State::DOWNLOADING) {
                const uint32_t percent =
                    ota.image_size == 0 ? 0U : static_cast<uint32_t>(ota.bytes_downloaded * 100 / ota.image_size);
                snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Source %.18s", ota.active_source);
                snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "Download %" PRIu32 "%%", percent);
                snprintf(detail_lines_[3], sizeof(detail_lines_[3]), "Do not power off");
            } else if (ota.state == OtaService::State::FAILED) {
                snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "%.26s", ota.last_error);
                snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "MAIN retry");
            } else if (ota.state == OtaService::State::UP_TO_DATE) {
                snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Latest %.15s", ota.current_version);
                snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "MAIN recheck");
            } else if (ota.state == OtaService::State::VERIFYING) {
                snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Verifying image");
                snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "Do not power off");
            } else if (ota.state == OtaService::State::RESTARTING) {
                snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Upgrade complete");
                snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "Restarting...");
            } else {
                snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Checking GitHub...");
                snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "Please wait");
            }
            snprintf(detail_lines_[3], sizeof(detail_lines_[3]), "SIDE close");
        }
    } else if (item == BlackboxInfo) {
        const uint32_t interval = BlackboxService::get_snapshot_interval_s();
        snprintf(detail_lines_[0], sizeof(detail_lines_[0]), "State %s", Blackbox::is_enabled() ? "ON" : "OFF");
        snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Used %" PRIu32 "/%" PRIu32,
                 static_cast<uint32_t>(Blackbox::count()), static_cast<uint32_t>(Blackbox::capacity()));
        if (interval == 0) {
            snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "Snapshot OFF");
        } else {
            snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "Snapshot %" PRIu32 "s", static_cast<uint32_t>(interval));
        }
    } else if (item == CalibrationInfo) {
        const auto params       = CurrentCalib::params_data.read();
        uint8_t    valid_points = 0;
        for (const auto& point : params.points) {
            if (point.register_value != 0 || point.offset_current_100uA != 0) {
                valid_points++;
            }
        }
        const float sample_resistance_mohm = params.current_base_K == 0 ? 0.0f : 2500.0f / params.current_base_K;
        snprintf(detail_lines_[0], sizeof(detail_lines_[0]), "calibration %s %" PRIu32 "/6",
                 valid_points == 6 ? "YES" : "NO",
                 static_cast<uint32_t>(valid_points));
        snprintf(detail_lines_[1], sizeof(detail_lines_[1]), "Resistance %.3fmR", sample_resistance_mohm);
        snprintf(detail_lines_[2], sizeof(detail_lines_[2]), "BaseK %" PRIu32,
                 static_cast<uint32_t>(params.current_base_K));
    }
}

/** @brief 绘制设置项弹窗。 */
void SettingsPage::draw_dialog_overlay() {
    build_dialog_content();
    ST7789::fill_round_rect(8, 8, 224, 119, 6, ST7789::BLACK, ST7789::BLACK);
    ST7789::draw_round_rect(8, 8, 224, 119, 6, 1, UI::YELLOW, ST7789::BLACK);
    UI::text(16, 13, 208, 20, item_name(selected_), UI::YELLOW);
    ST7789::fill_rect(16, 37, 208, 1, UI::GRID);
    for (uint8_t i = 0; i < 4; ++i) {
        UI::text(16, 42 + i * 20, 208, 18, detail_lines_[i], i < 2 ? ST7789::WHITE : UI::MUTED, ST7789::BLACK, DENGB16);
    }
}

/** @brief 修改当前选中的设置项。 */
void SettingsPage::adjust_selected_item() {
    switch (selected_) {
    case Rotate180:
        rotation_180_ = !rotation_180_;
        if (ui_config_set_rotation_180(rotation_180_) != ESP_OK) {
            rotation_180_ = !rotation_180_;
            ESP_LOGE(TAG, "failed to persist rotation setting");
            break;
        }
        ST7789::set_rotation(rotation_180_ ? ST7789::Rotation::HorizontalMirror : ST7789::Rotation::Horizontal);
        DEVICE_EVENT_I(TAG, "ui: config source=screen rotate_180=%" PRIu32,
                       static_cast<uint32_t>(rotation_180_ ? 1U : 0U));
        break;
    case Backlight:
        backlight_level_++;
        if (backlight_level_ > BACKLIGHT_LEVEL_COUNT) {
            backlight_level_ = 1;
        }
        if (ui_config_set_backlight_level(backlight_level_) != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist backlight setting");
            backlight_level_ = ui_config_get_backlight_level();
            break;
        }
        ST7789::set_backlight(backlight_value_from_level(backlight_level_));
        DEVICE_EVENT_I(TAG, "ui: config source=screen backlight_level=%" PRIu32,
                       static_cast<uint32_t>(backlight_level_));
        break;
    case WebBoot: {
        bool enabled = !WifiService::is_web_enabled_on_boot();
        WifiService::set_web_enabled_on_boot(enabled, TAG);
        break;
    }
    case ProtectBypass:
        protect_set_bypassed(!protect_is_bypassed(), TAG);
        break;
    case BlackboxSnapshot: {
        const uint32_t current = BlackboxService::get_snapshot_interval_s();
        uint32_t       next    = BLACKBOX_SNAPSHOT_INTERVALS_S[0];
        for (size_t i = 0; i < sizeof(BLACKBOX_SNAPSHOT_INTERVALS_S) / sizeof(BLACKBOX_SNAPSHOT_INTERVALS_S[0]); ++i) {
            if (BLACKBOX_SNAPSHOT_INTERVALS_S[i] == current) {
                next = BLACKBOX_SNAPSHOT_INTERVALS_S[(i + 1) % (sizeof(BLACKBOX_SNAPSHOT_INTERVALS_S) /
                                                                sizeof(BLACKBOX_SNAPSHOT_INTERVALS_S[0]))];
                break;
            }
        }
        if (BlackboxService::set_snapshot_interval_s(next, TAG) != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist blackbox snapshot interval");
        }
        break;
    }
    case CanBaudrate: {
        uint32_t current = CanCallback::CAN_BAUDRATE.read();
        uint32_t next    = CAN_BAUDRATES[0];
        for (size_t i = 0; i < sizeof(CAN_BAUDRATES) / sizeof(CAN_BAUDRATES[0]); ++i) {
            if (CAN_BAUDRATES[i] == current) {
                next = CAN_BAUDRATES[(i + 1) % (sizeof(CAN_BAUDRATES) / sizeof(CAN_BAUDRATES[0]))];
                break;
            }
        }
        if (CanCallback::CAN_BAUDRATE.set(next) != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist CAN baudrate");
            break;
        }
        DEVICE_EVENT_I(TAG, "can: config baud=%" PRIu32 " source=screen reboot_required=1", static_cast<uint32_t>(next));
        break;
    }
    case CanTerm: {
        const esp_err_t ret = CanResistor::instance().toggle();
        if (ret == ESP_OK) {
            DEVICE_STATE_I(TAG, "can: resistor source=screen state=%" PRIu32 " result=ok",
                           static_cast<uint32_t>(CanResistor::instance().get() ? 1U : 0U));
        } else {
            ESP_LOGE(TAG, "can: resistor source=screen result=%s", esp_err_to_name(ret));
        }
        break;
    }
    default:
        break;
    }
}

} // namespace SCREEN

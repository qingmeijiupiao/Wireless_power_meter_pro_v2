#include "boot_diagnostics.h"

#include <cstdarg>
#include <cstdio>
#include <bit>

#include "blackbox.h"
#include "blackbox_service.h"
#include "can_callback.h"
#include "can_resistor.h"
#include "current_calibration.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "global_state.h"
#include "hardware.h"
#include "protect.h"
#include "wifi_service.h"

namespace BootDiagnostics {
namespace {

void append_boot_line(const char* fmt, ...) {
    char    text[Blackbox::TEXT_BUFFER_SIZE] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    BlackboxService::append_text_event("%s", text);
    Blackbox::sync();
}

} // namespace

void append_stage(const char* stage) {
    append_boot_line("boot: stage=%s", stage == nullptr ? "unknown" : stage);
}

void append_hardware_config_failure(esp_err_t err) {
    append_boot_line("boot: hardware_config_init_failed err=%s", esp_err_to_name(err));
}

void append_system_boot_start() {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    BlackboxService::append_event("system: boot_start fw=%s build=%s hw_version=%u", app_desc->version, BUILD_TIME,
                                  static_cast<uint32_t>(get_hardware_version()));
}

void append_early() {
    const esp_app_desc_t*  app_desc   = esp_app_get_description();
    const esp_partition_t* running    = esp_ota_get_running_partition();
    uint32_t               flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);
    uint8_t sta_mac[6] = {};
    uint8_t ap_mac[6]  = {};
    esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
    esp_read_mac(ap_mac, ESP_MAC_WIFI_SOFTAP);
    const auto wifi        = WifiService::get_config();
    const auto calibration = CurrentCalib::params_data.read();

    append_boot_line("boot: reset_reason=%u fw=%s build=%s hw=%u", static_cast<uint32_t>(esp_reset_reason()),
                     app_desc->version, BUILD_TIME, static_cast<uint32_t>(get_hardware_version()));
    append_boot_line("boot: flash_bytes=%lu ota_label=%s ota_subtype=%u", static_cast<uint32_t>(flash_size),
                     running == nullptr ? "unknown" : running->label,
                     running == nullptr ? 0U : static_cast<uint32_t>(running->subtype));
    append_boot_line("boot: mac_sta=%02X:%02X:%02X:%02X:%02X:%02X mac_ap=%02X:%02X:%02X:%02X:%02X:%02X", sta_mac[0],
                     sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], ap_mac[0], ap_mac[1], ap_mac[2],
                     ap_mac[3], ap_mac[4], ap_mac[5]);
    append_boot_line("boot: can_id=0x%lx can_baud=%lu", static_cast<uint32_t>(CanCallback::CAN_ID.read()),
                     static_cast<uint32_t>(CanCallback::CAN_BAUDRATE.read()));
    append_boot_line("boot: wifi_boot=%u saved_ssid=%s", wifi.web_enabled_on_boot ? 1U : 0U,
                     wifi.ssid[0] == '\0' ? "(none)" : wifi.ssid);
    append_boot_line("boot: calib base_k=%u temperature_k=%d", static_cast<uint32_t>(calibration.current_base_K),
                     calibration.temperature_K);
    for (size_t i = 0; i < sizeof(calibration.points) / sizeof(calibration.points[0]); ++i) {
        append_boot_line("boot: calib_point index=%u reg=%d offset_100ua=%d", static_cast<uint32_t>(i),
                         calibration.points[i].register_value, calibration.points[i].offset_current_100uA);
    }
    for (uint8_t i = 0; i < protect_get_channel_count(); ++i) {
        protect_channel_info_t info = {};
        if (protect_get_channel_info(i, &info)) {
            append_boot_line(
                "boot: protect channel=%s warn_milli=%ld warn_rec_milli=%ld protect_milli=%ld protect_rec_milli=%ld",
                info.name, static_cast<int32_t>(info.threshold.warning_threshold * 1000.0f),
                static_cast<int32_t>(info.threshold.warning_recovery_threshold * 1000.0f),
                static_cast<int32_t>(info.threshold.protect_threshold * 1000.0f),
                static_cast<int32_t>(info.threshold.protect_recovery_threshold * 1000.0f));
        }
    }
}

void append_runtime() {
    const IP_t  ip           = WifiService::get_ip();
    const auto  global_state = get_global_state();
    const auto  flags_raw    = std::bit_cast<uint32_t>(global_state.flags);
    append_boot_line("boot: runtime can_resistor=%u wifi_mode=%u ip=%u.%u.%u.%u",
                     CanResistor::instance().get() ? 1U : 0U, static_cast<uint32_t>(WifiService::get_mode()), ip.octet1,
                     ip.octet2, ip.octet3, ip.octet4);
    append_boot_line("boot: runtime ina228_raw_i=%d ina228_raw_v=%u flags=0x%lx", global_state.current_register_raw,
                     global_state.voltage_register_raw, static_cast<uint32_t>(flags_raw));
}

} // namespace BootDiagnostics

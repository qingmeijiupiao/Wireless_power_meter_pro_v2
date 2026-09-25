/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: Web 后端 REST API handler 实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-05-29
 */
#include "web_backend_internal.h"
#include "web_backend.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cinttypes>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "can_callback.h"
#include "current_calibration.h"
#include "energy_meter.h"
#include "global_state.h"
#include "hardware.h"
#include "ota_manager.h"
#include "power_output.h"
#include "protect.h"
#include "screen.h"
#include "st7789.h"
#include "wifi_service.h"
#include "espnow_link.h"
#include "espnow_service.h"
#include "diagnostic_log.h"

namespace WebBackend {

static constexpr char   TAG[]                = "WebBackendApi";
static constexpr size_t LOG_RESPONSE_RAW_MAX = 2400;
static char             log_snapshot_buffer[LOG_RESPONSE_RAW_MAX + 1];

static bool append_checked(char* out, size_t out_size, size_t* pos, const char* fmt, ...);

/** @brief HTTP 响应发出后再关闭 Web 和 IP 网络，避免在 handler 内销毁当前连接。 */
static void wifi_off_deferred_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(100));
    WebBackend::stop();
    const esp_err_t ret = WifiService::stop(TAG);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "IP network stopped; ESP-NOW remains active");
    } else {
        ESP_LOGW(TAG, "deferred network stop failed: %s", esp_err_to_name(ret));
    }
    vTaskDelete(nullptr);
}

/** @brief 将 WiFiService 模式转换为 API 字符串。 */
const char* mode_to_str(WifiService::Mode mode) {
    switch (mode) {
    case WifiService::Mode::OFF:
        return "off";
    case WifiService::Mode::ESPNOW_ONLY:
        return "espnow_only";
    case WifiService::Mode::STA:
        return "sta";
    case WifiService::Mode::AP_PROVISION:
        return "ap_provision";
    default:
        return "unknown";
    }
}

/** @brief 将 PowerOutput 结果转换为 API reason 字符串。 */
static const char* output_result_to_str(PowerOutput::OutputResult result) {
    return PowerOutput::result_to_string(result);
}

/** @brief 将 IP_t 转换为点分十进制字符串。 */
void ip_to_str(IP_t ip, char* out, size_t out_size) {
    snprintf(out, out_size, "%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32, static_cast<uint32_t>(ip.octet1),
             static_cast<uint32_t>(ip.octet2), static_cast<uint32_t>(ip.octet3), static_cast<uint32_t>(ip.octet4));
}

/** @brief 将 MAC_t 转换为常见冒号分隔字符串。 */
static void mac_to_str(MAC_t mac, char* out, size_t out_size) {
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X", mac.octet1, mac.octet2, mac.octet3, mac.octet4, mac.octet5,
             mac.octet6);
}

/** @brief 从统一 BUILD_TIME 中拆出 API 兼容的日期和时钟字段。 */
static void split_build_time(char* build_date, size_t build_date_size, char* build_clock, size_t build_clock_size) {
    // BUILD_TIME is fixed by CMake as UTC+8 "YYYY/MM/DD HH:MM:SS" for consistent local/CD display.
    snprintf(build_date, build_date_size, "%.10s", BUILD_TIME);
    snprintf(build_clock, build_clock_size, "%.8s", BUILD_TIME + 11);
}

/**
 * @brief GET /api/state
 *
 * 返回首页高频轮询所需的实时测量值、输出状态、保护摘要和 WiFi 摘要。
 * 该接口保持短响应，避免首页刷新时占用过多 HTTP 任务栈和发送缓冲。
 */
esp_err_t state_handler(WebServer::Request* request) {
    auto  state       = get_global_state();
    IP_t  ip          = WifiService::get_ip();
    char  ip_text[16] = {};
    ip_to_str(ip, ip_text, sizeof(ip_text));

    float                       voltage_v     = state.voltage_mV / 1000.0f;
    float                       current_a     = state.current_uA / 1000000.0f;
    float                       abs_current_a = std::abs(state.current_uA) / 1000000.0f;
    float                       board_temp_c  = state.board_temperature / 100.0f;
    float                       chip_temp_c   = state.chip_temperature / 100.0f;
    auto&                       protect       = state.protect_states.states_bit;
    const EnergyMeter::Snapshot meter         = EnergyMeter::snapshot();

    snprintf(response_buffer, sizeof(response_buffer),
             "{"
             "\"voltage_v\":%.3f,"
             "\"current_a\":%.3f,"
             "\"power_w\":%.3f,"
             "\"board_temp_c\":%.2f,"
             "\"chip_temp_c\":%.2f,"
             "\"energy_mwh\":%.3f,"
             "\"charge_mah\":%.3f,"
             "\"meter_time_ms\":%" PRIu64 ","
             "\"output_on\":%s,"
             "\"protect_bypassed\":%s,"
             "\"uptime_ms\":%" PRId64 ","
             "\"protect\":{\"otp\":%" PRIu32 ",\"ovp\":%" PRIu32 ",\"uvp\":%" PRIu32 ",\"ocp\":%" PRIu32 "},"
             "\"wifi\":{\"mode\":\"%s\",\"state\":%d,\"ip\":\"%s\",\"ap_ssid\":\"%s\",\"boot_enabled\":%s,\"last_"
             "error\":\"%s\"}"
             "}\n",
             voltage_v, current_a, voltage_v * abs_current_a, board_temp_c, chip_temp_c, meter.energy_uwh / 1000.0,
             meter.charge_uah / 1000.0, meter.meter_time_ms, state.flags.output_enabled ? "true" : "false",
             protect_is_bypassed() ? "true" : "false", static_cast<int64_t>(esp_timer_get_time() / 1000),
             static_cast<uint32_t>(protect.temperature_protect_state),
             static_cast<uint32_t>(protect.high_voltage_protect_state),
             static_cast<uint32_t>(protect.low_voltage_protect_state),
             static_cast<uint32_t>(protect.current_protect_state),
             mode_to_str(WifiService::get_mode()), (int)WifiService::get_wifi_state(), ip_text,
             WifiService::get_ap_ssid(), WifiService::is_web_enabled_on_boot() ? "true" : "false",
             WifiService::get_last_error());

    return WebServer::send_json(request, response_buffer);
}

/** @brief POST /api/meter/reset, reset the shared UI/Web metering session. */
esp_err_t meter_reset_handler(WebServer::Request* request) {
    EnergyMeter::reset();
    DEVICE_EVENT_I(TAG, "meter: reset source=web");
    return WebServer::send_json(request, "{\"ok\":true}\n");
}

/**
 * @brief POST /api/output
 *
 * 请求体示例：`{"state":true}` 或 `{"toggle":true}`。
 * 实际保护和冷却判断全部交给 PowerOutput，Web 层不复制业务规则。
 */
esp_err_t output_handler(WebServer::Request* request) {
    esp_err_t ret = WebServer::load_body(request);
    if (ret != ESP_OK) {
        return ret;
    }

    bool                      target    = false;
    bool                      has_state = json_get_bool(request->body, "state", &target);
    PowerOutput::OutputResult result    = PowerOutput::OutputResult::OK;
    if (json_has_key(request->body, "toggle")) {
        result = PowerOutput::toggle(TAG);
    } else if (has_state) {
        result = target ? PowerOutput::on(TAG) : PowerOutput::off(TAG);
    } else {
        ESP_LOGW(TAG, "output request rejected: missing state");
        return WebServer::send(request, 400, "application/json", "{\"ok\":false,\"reason\":\"missing_state\"}\n",
                               strlen("{\"ok\":false,\"reason\":\"missing_state\"}\n"));
    }

    if (result == PowerOutput::OutputResult::OK) {
        ESP_LOGI(TAG, "output updated: state=%s", PowerOutput::get_state() ? "on" : "off");
    } else {
        ESP_LOGW(TAG, "output update rejected: reason=%s", output_result_to_str(result));
    }
    snprintf(response_buffer, sizeof(response_buffer), "{\"ok\":%s,\"reason\":\"%s\",\"output_on\":%s}\n",
             result == PowerOutput::OutputResult::OK ? "true" : "false", output_result_to_str(result),
             PowerOutput::get_state() ? "true" : "false");
    return WebServer::send_json(request, response_buffer);
}

/** @brief 延迟重启回调，确保 HTTP 响应有机会先发回浏览器。 */
static void reboot_timer_callback(void* arg) {
    esp_restart();
}

/** @brief POST /api/reboot，返回响应后约 300ms 重启设备。 */
esp_err_t reboot_handler(WebServer::Request* request) {
    static esp_timer_handle_t reboot_timer = nullptr;
    if (reboot_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback                = reboot_timer_callback;
        args.name                    = "web_reboot";
        esp_err_t ret                = esp_timer_create(&args, &reboot_timer);
        if (ret != ESP_OK) {
            snprintf(response_buffer, sizeof(response_buffer), "{\"ok\":false,\"reason\":\"%s\"}\n",
                     esp_err_to_name(ret));
            return WebServer::send_json(request, response_buffer);
        }
    }
    esp_timer_stop(reboot_timer);
    esp_timer_start_once(reboot_timer, 300000);
    DEVICE_STATE_W(TAG, "system: reboot source=web delay_ms=300 ip=%s", request->peer_ip);
    return WebServer::send_json(request, "{\"ok\":true,\"reason\":\"rebooting\"}\n");
}

/** @brief GET /api/system，返回固件版本、构建信息、MAC 和运行时间。 */
esp_err_t system_handler(WebServer::Request* request) {
    const esp_app_desc_t*  app_desc          = esp_app_get_description();
    const esp_partition_t* running_partition = OtaManager::get_running_partition();
    char                   sta_mac[18]       = {};
    char                   ap_mac[18]        = {};
    char                   build_date[11]    = {};
    char                   build_clock[9]    = {};
    mac_to_str(WiFiManager::instance().get_mac(WIFI_IF_STA), sta_mac, sizeof(sta_mac));
    mac_to_str(WiFiManager::instance().get_mac(WIFI_IF_AP), ap_mac, sizeof(ap_mac));
    split_build_time(build_date, sizeof(build_date), build_clock, sizeof(build_clock));
    snprintf(detail_response_buffer, sizeof(detail_response_buffer),
             "{"
             "\"hardware_version\":%" PRIu32 ","
             "\"firmware\":{\"major\":%" PRIu32 ",\"minor\":%" PRIu32 ",\"patch\":%" PRIu32
             ",\"project\":\"%s\",\"build\":\"%s\",\"build_date\":"
             "\"%s\",\"build_time\":\"%s\"},"
             "\"app_partition\":{\"slot\":%" PRIu32 ",\"label\":\"%s\"},"
             "\"mac\":{\"sta\":\"%s\",\"ap\":\"%s\"},"
             "\"uptime_ms\":%" PRId64
             "}\n",
             static_cast<uint32_t>(get_hardware_version()), static_cast<uint32_t>(VERSION_MAJOR),
             static_cast<uint32_t>(VERSION_MINOR), static_cast<uint32_t>(VERSION_PATCH), app_desc->project_name,
             BUILD_TIME, build_date, build_clock, static_cast<uint32_t>(ota_partition_slot(running_partition)),
             running_partition == nullptr ? "" : running_partition->label, sta_mac, ap_mac,
             static_cast<int64_t>(esp_timer_get_time() / 1000));
    return WebServer::send_json(request, detail_response_buffer);
}

/**
 * @brief GET/POST /api/backlight
 *
 * GET 查询当前背光；POST 通过 `{"brightness":0..255}` 设置背光。
 */
esp_err_t backlight_handler(WebServer::Request* request) {
    if (request->method == WebServer::Method::POST) {
        esp_err_t ret = WebServer::load_body(request);
        if (ret != ESP_OK) {
            return ret;
        }
        uint32_t brightness = 0;
        if (!json_get_uint32(request->body, "brightness", &brightness) || brightness > 255) {
            return WebServer::send(request, 400, "application/json",
                                   "{\"ok\":false,\"reason\":\"invalid_brightness\"}\n",
                                   strlen("{\"ok\":false,\"reason\":\"invalid_brightness\"}\n"));
        }
        ret = ST7789::set_backlight(static_cast<uint8_t>(brightness));
        if (ret == ESP_OK) {
            DEVICE_EVENT_I(TAG, "ui: config source=web backlight=%" PRIu32 " ip=%s",
                           static_cast<uint32_t>(brightness),
                           request->peer_ip);
        } else {
            ESP_LOGW(TAG, "backlight update failed: brightness=%" PRIu32 " reason=%s", brightness,
                     esp_err_to_name(ret));
        }
        snprintf(response_buffer, sizeof(response_buffer),
                 "{\"ok\":%s,\"reason\":\"%s\",\"brightness\":%" PRIu32 "}\n",
                 ret == ESP_OK ? "true" : "false", ret == ESP_OK ? "ok" : esp_err_to_name(ret),
                 static_cast<uint32_t>(ST7789::get_backlight()));
        return WebServer::send_json(request, response_buffer);
    }

    snprintf(response_buffer, sizeof(response_buffer), "{\"brightness\":%" PRIu32 "}\n",
             static_cast<uint32_t>(ST7789::get_backlight()));
    return WebServer::send_json(request, response_buffer);
}

/** @brief GET/POST /api/start-logo, query or persist the startup logo duration. */
esp_err_t start_logo_handler(WebServer::Request* request) {
    if (request->method == WebServer::Method::POST) {
        esp_err_t ret = WebServer::load_body(request);
        if (ret != ESP_OK) {
            return ret;
        }

        uint32_t duration_ms = 0;
        if (!json_get_uint32(request->body, "duration_ms", &duration_ms) ||
            duration_ms > SCREEN::MAX_START_LOGO_DURATION_MS) {
            return WebServer::send(request, 400, "application/json",
                                   "{\"ok\":false,\"reason\":\"invalid_duration_ms\"}\n",
                                   strlen("{\"ok\":false,\"reason\":\"invalid_duration_ms\"}\n"));
        }

        ret = SCREEN::set_start_logo_duration_ms(duration_ms);
        if (ret != ESP_OK) {
            snprintf(response_buffer, sizeof(response_buffer), "{\"ok\":false,\"reason\":\"%s\"}\n",
                     esp_err_to_name(ret));
            return WebServer::send(request, 500, "application/json", response_buffer, strlen(response_buffer));
        }
        DEVICE_EVENT_I(TAG, "ui: config source=web start_logo_ms=%" PRIu32 " reboot_required=1 ip=%s",
                       static_cast<uint32_t>(duration_ms), request->peer_ip);
    }

    const uint32_t duration_ms = SCREEN::get_start_logo_duration_ms();
    snprintf(response_buffer, sizeof(response_buffer),
             "{\"ok\":true,\"duration_ms\":%" PRIu32
             ",\"enabled\":%s,\"note\":\"changed value takes effect after reboot\"}\n",
             static_cast<uint32_t>(duration_ms), duration_ms > 0 ? "true" : "false");
    return WebServer::send_json(request, response_buffer);
}

/** @brief 将保护状态枚举转换为前端可读字符串。 */
static const char* protect_state_to_str(ProtectState_t state) {
    switch (state) {
    case PROTECT_STATE_NORMAL:
        return "normal";
    case PROTECT_STATE_WARNING:
        return "warning";
    case PROTECT_STATE_PROTECT:
        return "protect";
    default:
        return "unknown";
    }
}

/**
 * @brief GET/POST /api/protect
 *
 * GET 返回保护通道详情；POST 可开关保护阻断或更新指定通道阈值。
 * 即使旁路保护，protect 模块仍会持续检测和上报故障状态。
 */
esp_err_t protect_handler(WebServer::Request* request) {
    if (request->method == WebServer::Method::POST) {
        esp_err_t ret = WebServer::load_body(request);
        if (ret != ESP_OK) {
            return ret;
        }
        const bool has_enabled = json_has_key(request->body, "enabled");
        const bool has_channel = json_has_key(request->body, "channel");
        if (has_enabled && has_channel) {
            return WebServer::send(request, 400, "application/json",
                                   "{\"ok\":false,\"reason\":\"combined_update_not_allowed\"}\n",
                                   strlen("{\"ok\":false,\"reason\":\"combined_update_not_allowed\"}\n"));
        }
        bool updated = false;
        if (has_enabled) {
            bool enabled = true;
            if (!json_get_bool(request->body, "enabled", &enabled)) {
                return WebServer::send(request, 400, "application/json",
                                       "{\"ok\":false,\"reason\":\"invalid_enabled\"}\n",
                                       strlen("{\"ok\":false,\"reason\":\"invalid_enabled\"}\n"));
            }
            protect_set_bypassed(!enabled, TAG);
            ESP_LOGI(TAG, "protection updated: enabled=%s", enabled ? "true" : "false");
            if (enabled && protect_should_block_output()) {
                ESP_LOGW(TAG, "protection enabled with active fault, forcing output off");
                PowerOutput::off(TAG);
            }
            updated = true;
        }

        if (has_channel) {
            uint32_t               channel                = 0;
            uint32_t               warning_milli          = 0;
            uint32_t               warning_recovery_milli = 0;
            uint32_t               protect_milli          = 0;
            uint32_t               protect_recovery_milli = 0;
            protect_channel_info_t info                   = {};
            if (!json_get_uint32(request->body, "channel", &channel) || channel >= protect_get_channel_count() ||
                !json_get_uint32(request->body, "warning_milli", &warning_milli) ||
                !json_get_uint32(request->body, "warning_recovery_milli", &warning_recovery_milli) ||
                !json_get_uint32(request->body, "protect_milli", &protect_milli) ||
                !json_get_uint32(request->body, "protect_recovery_milli", &protect_recovery_milli) ||
                !protect_get_channel_info(static_cast<uint8_t>(channel), &info)) {
                return WebServer::send(request, 400, "application/json",
                                       "{\"ok\":false,\"reason\":\"invalid_threshold_fields\"}\n",
                                       strlen("{\"ok\":false,\"reason\":\"invalid_threshold_fields\"}\n"));
            }

            protect_threshold_t threshold        = info.threshold;
            threshold.warning_threshold          = warning_milli / 1000.0f;
            threshold.warning_recovery_threshold = warning_recovery_milli / 1000.0f;
            threshold.protect_threshold          = protect_milli / 1000.0f;
            threshold.protect_recovery_threshold = protect_recovery_milli / 1000.0f;
            if (protect_set_channel_threshold(static_cast<uint8_t>(channel), threshold, TAG) != ESP_OK) {
                return WebServer::send(request, 400, "application/json",
                                       "{\"ok\":false,\"reason\":\"invalid_threshold_order\"}\n",
                                       strlen("{\"ok\":false,\"reason\":\"invalid_threshold_order\"}\n"));
            }
            updated = true;
        }

        if (!updated) {
            return WebServer::send(request, 400, "application/json", "{\"ok\":false,\"reason\":\"missing_update\"}\n",
                                   strlen("{\"ok\":false,\"reason\":\"missing_update\"}\n"));
        }
    }

    size_t pos = 0;
    bool   ok =
        append_checked(detail_response_buffer, sizeof(detail_response_buffer), &pos,
                       "{\"enabled\":%s,\"bypassed\":%s,\"active_fault\":%s,\"should_block_output\":%s,\"channels\":[",
                       protect_is_bypassed() ? "false" : "true", protect_is_bypassed() ? "true" : "false",
                       protect_has_active_fault() ? "true" : "false", protect_should_block_output() ? "true" : "false");

    for (uint8_t i = 0; ok && i < protect_get_channel_count(); ++i) {
        protect_channel_info_t info = {};
        if (!protect_get_channel_info(i, &info)) {
            continue;
        }
        ok = append_checked(
            detail_response_buffer, sizeof(detail_response_buffer), &pos,
            "%s{\"name\":\"%s\",\"unit\":\"%s\",\"value\":%.3f,\"state\":%" PRIu32 ",\"state_text\":\"%s\","
            "\"warning\":%.3f,\"warning_recovery\":%.3f,\"protect\":%.3f,\"protect_recovery\":%.3f,\"trigger\":\"%s\"}",
            i == 0 ? "" : ",", info.name, info.unit, info.now_value, static_cast<uint32_t>(info.state),
            protect_state_to_str(info.state), info.threshold.warning_threshold,
            info.threshold.warning_recovery_threshold, info.threshold.protect_threshold,
            info.threshold.protect_recovery_threshold, info.threshold.is_asc ? ">=" : "<=");
    }

    if (!ok || !append_checked(detail_response_buffer, sizeof(detail_response_buffer), &pos, "]}\n")) {
        snprintf(detail_response_buffer, sizeof(detail_response_buffer), "{\"error\":\"response_too_large\"}\n");
    }
    return WebServer::send_json(request, detail_response_buffer);
}

/**
 * @brief GET/POST /api/can
 *
 * 查询或修改 CAN 运行参数。当前只更新运行期变量，是否需要重启或重新初始化由上层提示。
 */
esp_err_t can_handler(WebServer::Request* request) {
    const bool is_post = request->method == WebServer::Method::POST;
    if (is_post) {
        esp_err_t ret = WebServer::load_body(request);
        if (ret != ESP_OK) {
            return ret;
        }
        uint32_t       baudrate          = 0;
        uint32_t       id                = 0;
        const uint32_t previous_baudrate = CanCallback::CAN_BAUDRATE.read();
        if (json_get_uint32(request->body, "baudrate", &baudrate)) {
            if (baudrate == 0) {
                return WebServer::send(request, 400, "application/json",
                                       "{\"ok\":false,\"reason\":\"invalid_baudrate\"}\n",
                                       strlen("{\"ok\":false,\"reason\":\"invalid_baudrate\"}\n"));
            }
            ret = CanCallback::CAN_BAUDRATE.set(baudrate);
            if (ret != ESP_OK) {
                return WebServer::send(request, 500, "application/json",
                                       "{\"ok\":false,\"reason\":\"persist_failed\"}\n",
                                       strlen("{\"ok\":false,\"reason\":\"persist_failed\"}\n"));
            }
        }
        if (json_get_uint32(request->body, "id", &id)) {
            ret = CanCallback::CAN_ID.set(id);
            if (ret != ESP_OK) {
                if (baudrate != 0) {
                    CanCallback::CAN_BAUDRATE.set(previous_baudrate);
                }
                return WebServer::send(request, 500, "application/json",
                                       "{\"ok\":false,\"reason\":\"persist_failed\"}\n",
                                       strlen("{\"ok\":false,\"reason\":\"persist_failed\"}\n"));
            }
        }
    }

    uint32_t can_id   = CanCallback::CAN_ID;
    uint32_t baudrate = CanCallback::CAN_BAUDRATE;
    if (is_post) {
        DEVICE_EVENT_I(TAG, "can: config baud=%" PRIu32 " id=0x%" PRIx32 " source=web reboot_required=1",
                       static_cast<uint32_t>(baudrate), static_cast<uint32_t>(can_id));
    }
    snprintf(response_buffer, sizeof(response_buffer),
             "{\"ok\":true,\"baudrate\":%" PRIu32 ",\"id\":%" PRIu32 ",\"id_hex\":\"0x%" PRIX32
             "\",\"note\":\"changed values may require CAN "
             "reinitialization or reboot\"}\n",
             baudrate, can_id, can_id);
    return WebServer::send_json(request, response_buffer);
}

namespace {
// 校准参数合法范围，与 LP 核使用的字段类型保持一致。
constexpr uint32_t CALIB_BASE_K_MIN        = 1;
constexpr uint32_t CALIB_BASE_K_MAX        = 65535;
constexpr uint32_t CALIB_CURRENT_MA_MAX    = 100000; // 100 A
constexpr uint32_t CALIB_REGISTER_RAW_MAX  = 32767;
constexpr int32_t  CALIB_TEMPERATURE_K_MIN = -32767;
constexpr int32_t  CALIB_TEMPERATURE_K_MAX = 32767;
constexpr size_t   CALIB_POINT_COUNT       = 6;

esp_err_t calibration_bad_request(WebServer::Request* request, const char* reason) {
    snprintf(response_buffer, sizeof(response_buffer), "{\"ok\":false,\"reason\":\"%s\"}\n", reason);
    return WebServer::send(request, 400, "application/json", response_buffer, strlen(response_buffer));
}

bool append_calibration_fields(char* out, size_t out_size, size_t* pos, const CurrentCalib::params_t& params) {
    const float sample_resistance_mohm = params.current_base_K == 0 ? 0.0f : 2500.0f / params.current_base_K;
    bool        ok                     = append_checked(
        out, out_size, pos,
        "\"current_base_k\":%" PRIu32 ",\"sample_resistance_mohm\":%.3f,\"temperature_k\":%d,"
        "\"base_temperature_c\":%.2f,\"points\":[",
        static_cast<uint32_t>(params.current_base_K), sample_resistance_mohm, params.temperature_K,
        CurrentCalib::BASE_TEMPERATURE / 100.0f);

    for (size_t i = 0; ok && i < CALIB_POINT_COUNT; ++i) {
        ok = append_checked(out, out_size, pos,
                            "%s{\"index\":%" PRIu32
                            ",\"register_value\":%d,\"no_offset_ma\":%d,\"offset_ua\":%d}",
                            i == 0 ? "" : ",", static_cast<uint32_t>(i), params.points[i].register_value,
                            params.points[i].register_value * params.current_base_K / 1000,
                            params.points[i].offset_current_100uA * 100);
    }
    return ok && append_checked(out, out_size, pos, "]");
}
} // namespace

/**
 * @brief GET/POST /api/calibration
 *
 * GET 返回电流校准参数快照。POST 支持四类操作，可单独或组合提交：
 * - `base_k` 直接写入 K；`real_current_ma` 按当前 shunt 原始值绝对值自动计算 K。
 * - `temperature_k` 直接写入温漂系数；`temp_display_current_ma` + `temp_real_current_ma`
 *   按当前板温自动计算温漂系数。
 * - `point_index` + `point_register_raw` + `point_real_current_ma` 用当前 K 计算并写入插值点。
 * - `reset_secondary` 清除插值点与温漂系数（保留 K）。
 *
 * 校准结果写入 NVS，需要重启后由 LP 核重新加载才会生效，响应以 `reboot_required` 提示。
 */
esp_err_t calibration_handler(WebServer::Request* request) {
    if (request->method == WebServer::Method::POST) {
        esp_err_t ret = WebServer::load_body(request);
        if (ret != ESP_OK) {
            return ret;
        }
        const char* body = request->body;

        CurrentCalib::params_t params                   = CurrentCalib::params_data.read();
        bool                   any_change               = false;
        int32_t                computed_base_k          = -1;
        int32_t                computed_temperature_k   = 0;
        bool                   temperature_computed     = false;

        bool reset_secondary = false;
        if (json_get_bool(body, "reset_secondary", &reset_secondary) && reset_secondary) {
            memset(params.points, 0, sizeof(params.points));
            params.temperature_K = 0;
            any_change           = true;
        }

        uint32_t base_k = 0;
        if (json_get_uint32(body, "base_k", &base_k)) {
            if (base_k < CALIB_BASE_K_MIN || base_k > CALIB_BASE_K_MAX) {
                return calibration_bad_request(request, "invalid_base_k");
            }
            params.current_base_K = static_cast<uint16_t>(base_k);
            any_change            = true;
        }

        uint32_t real_current_ma = 0;
        if (json_get_uint32(body, "real_current_ma", &real_current_ma)) {
            if (real_current_ma < 1 || real_current_ma > CALIB_CURRENT_MA_MAX) {
                return calibration_bad_request(request, "invalid_real_current");
            }
            const int16_t raw = get_global_state().current_register_raw;
            if (raw == 0) {
                return calibration_bad_request(request, "register_raw_unavailable");
            }
            // K 为正的 uA/LSB 系数，使用原始值绝对值，兼容采样方向反向导致的负值。
            const int32_t raw_magnitude = raw < 0 ? -static_cast<int32_t>(raw) : static_cast<int32_t>(raw);
            const int64_t real_ua       = static_cast<int64_t>(real_current_ma) * 1000;
            const int64_t k             = (real_ua + raw_magnitude / 2) / raw_magnitude;
            if (k < CALIB_BASE_K_MIN || k > CALIB_BASE_K_MAX) {
                return calibration_bad_request(request, "computed_base_k_out_of_range");
            }
            params.current_base_K = static_cast<uint16_t>(k);
            computed_base_k       = static_cast<int32_t>(k);
            any_change            = true;
        }

        int32_t temperature_k = 0;
        if (json_get_int32(body, "temperature_k", &temperature_k)) {
            if (temperature_k < CALIB_TEMPERATURE_K_MIN || temperature_k > CALIB_TEMPERATURE_K_MAX) {
                return calibration_bad_request(request, "invalid_temperature_k");
            }
            params.temperature_K = static_cast<int16_t>(temperature_k);
            any_change           = true;
        }

        uint32_t   temp_display_ma   = 0;
        uint32_t   temp_real_ma      = 0;
        const bool has_temp_display  = json_get_uint32(body, "temp_display_current_ma", &temp_display_ma);
        const bool has_temp_real     = json_get_uint32(body, "temp_real_current_ma", &temp_real_ma);
        if (has_temp_display || has_temp_real) {
            if (!has_temp_display || !has_temp_real) {
                return calibration_bad_request(request, "temperature_requires_display_and_real");
            }
            if (temp_display_ma == 0 || temp_real_ma == 0 || temp_real_ma > CALIB_CURRENT_MA_MAX ||
                temp_display_ma > CALIB_CURRENT_MA_MAX) {
                return calibration_bad_request(request, "invalid_temperature_current");
            }
            const int32_t board_temperature = get_global_state().board_temperature; // 0.01℃
            const int32_t delta_temp_c      = (board_temperature - CurrentCalib::BASE_TEMPERATURE) / 100;
            if (delta_temp_c == 0) {
                return calibration_bad_request(request, "temperature_delta_zero");
            }
            const int64_t numerator =
                (static_cast<int64_t>(temp_display_ma) - static_cast<int64_t>(temp_real_ma)) * 1000000;
            const int64_t denominator = static_cast<int64_t>(temp_real_ma) * delta_temp_c;
            const int64_t drift_ppm   = numerator >= 0 ? (numerator + denominator / 2) / denominator
                                                       : (numerator - denominator / 2) / denominator;
            if (drift_ppm < CALIB_TEMPERATURE_K_MIN || drift_ppm > CALIB_TEMPERATURE_K_MAX) {
                return calibration_bad_request(request, "computed_temperature_k_out_of_range");
            }
            params.temperature_K   = static_cast<int16_t>(drift_ppm);
            computed_temperature_k = static_cast<int32_t>(drift_ppm);
            temperature_computed   = true;
            any_change             = true;
        }

        uint32_t   point_index     = 0;
        uint32_t   point_raw       = 0;
        uint32_t   point_real_ma   = 0;
        const bool has_point_index = json_get_uint32(body, "point_index", &point_index);
        const bool has_point_raw   = json_get_uint32(body, "point_register_raw", &point_raw);
        const bool has_point_real  = json_get_uint32(body, "point_real_current_ma", &point_real_ma);
        if (has_point_index || has_point_raw || has_point_real) {
            if (!has_point_index || !has_point_raw || !has_point_real) {
                return calibration_bad_request(request, "point_requires_index_register_and_current");
            }
            if (point_index >= CALIB_POINT_COUNT) {
                return calibration_bad_request(request, "invalid_point_index");
            }
            if (point_raw < 1 || point_raw > CALIB_REGISTER_RAW_MAX) {
                return calibration_bad_request(request, "invalid_point_register");
            }
            if (point_real_ma < 1 || point_real_ma > CALIB_CURRENT_MA_MAX) {
                return calibration_bad_request(request, "invalid_point_current");
            }
            const int64_t real_ua   = static_cast<int64_t>(point_real_ma) * 1000;
            const int64_t linear_ua = static_cast<int64_t>(params.current_base_K) * static_cast<int64_t>(point_raw);
            const int64_t offset_ua = real_ua - linear_ua;
            const int64_t offset_100ua = offset_ua >= 0 ? (offset_ua + 50) / 100 : (offset_ua - 50) / 100;
            if (offset_100ua < -32768 || offset_100ua > 32767) {
                return calibration_bad_request(request, "point_offset_out_of_range");
            }
            params.points[point_index].register_value       = static_cast<int16_t>(point_raw);
            params.points[point_index].offset_current_100uA = static_cast<int16_t>(offset_100ua);
            any_change                                      = true;
        }

        if (!any_change) {
            return calibration_bad_request(request, "missing_update");
        }

        const esp_err_t persist_ret = CurrentCalib::params_data.set(params);
        if (persist_ret != ESP_OK) {
            return WebServer::send(request, 500, "application/json", "{\"ok\":false,\"reason\":\"persist_failed\"}\n",
                                   strlen("{\"ok\":false,\"reason\":\"persist_failed\"}\n"));
        }
        DEVICE_EVENT_I(TAG, "calib: web base_k=%u temperature_k=%d source=web reboot_required=1",
                       static_cast<uint32_t>(params.current_base_K), params.temperature_K);

        size_t pos = 0;
        bool   ok  = append_checked(response_buffer, sizeof(response_buffer), &pos,
                                    "{\"ok\":true,\"reboot_required\":true,\"computed_base_k\":%d,"
                                    "\"computed_temperature_k\":%d,\"temperature_computed\":%s,",
                                    computed_base_k, computed_temperature_k, temperature_computed ? "true" : "false");
        ok = ok && append_calibration_fields(response_buffer, sizeof(response_buffer), &pos, params);
        ok = ok && append_checked(response_buffer, sizeof(response_buffer), &pos, "}\n");
        if (!ok) {
            snprintf(response_buffer, sizeof(response_buffer), "{\"ok\":false,\"reason\":\"response_too_large\"}\n");
            return WebServer::send(request, 500, "application/json", response_buffer, strlen(response_buffer));
        }
        return WebServer::send_json(request, response_buffer);
    }

    const CurrentCalib::params_t params = CurrentCalib::params_data.read();
    size_t                       pos    = 0;
    bool                         ok     = append_checked(detail_response_buffer, sizeof(detail_response_buffer), &pos, "{");
    ok = ok && append_calibration_fields(detail_response_buffer, sizeof(detail_response_buffer), &pos, params);
    ok = ok && append_checked(detail_response_buffer, sizeof(detail_response_buffer), &pos, "}\n");
    if (!ok) {
        snprintf(detail_response_buffer, sizeof(detail_response_buffer), "{\"error\":\"response_too_large\"}\n");
    }
    return WebServer::send_json(request, detail_response_buffer);
}

/** @brief GET /api/diagnostics，返回底层采样寄存器等诊断数据。 */
esp_err_t diagnostics_handler(WebServer::Request* request) {
    const auto state = get_global_state();
    if (!state.flags.lp_ina228_initialized) {
        ESP_LOGW(TAG, "INA228 diagnostics unavailable");
    }
    twai_node_status_t can_status         = {};
    twai_node_record_t can_statistics     = {};
    HXC_TWAI*          can                = CanCallback::is_available() ? &CanCallback::get_can_bus() : nullptr;
    const bool         can_info_available = can != nullptr && can->get_info(&can_status, &can_statistics) == ESP_OK;
    snprintf(response_buffer, sizeof(response_buffer),
             "{\"ina228\":{\"current_register_raw\":%d,\"voltage_register_raw\":%" PRIu32
             ",\"available\":%s},\"can\":{\"info_"
             "available\":%s,\"state\":%" PRIu32 ",\"tx_error_count\":%" PRIu32
             ",\"rx_error_count\":%" PRIu32 ",\"bus_error_count\":%" PRIu32 ",\"bus_"
             "off_count\":%" PRIu32 ",\"tx_failed_count\":%" PRIu32 ",\"rx_overflow_count\":%" PRIu32 "}}\n",
             state.current_register_raw, static_cast<uint32_t>(state.voltage_register_raw),
             state.flags.lp_ina228_initialized ? "true" : "false", can_info_available ? "true" : "false",
             static_cast<uint32_t>(can_status.state), static_cast<uint32_t>(can_status.tx_error_count),
             static_cast<uint32_t>(can_status.rx_error_count), static_cast<uint32_t>(can_statistics.bus_err_num),
             can == nullptr ? static_cast<uint32_t>(0) : static_cast<uint32_t>(can->get_bus_off_count()),
             can == nullptr ? static_cast<uint32_t>(0) : static_cast<uint32_t>(can->get_tx_failed_count()),
             can == nullptr ? static_cast<uint32_t>(0) : static_cast<uint32_t>(can->get_rx_overflow_count()));
    return WebServer::send_json(request, response_buffer);
}

/** @brief GET /api/wifi/status，返回 WiFi、Web 和 ESP-NOW 共用射频状态。 */
esp_err_t wifi_status_handler(WebServer::Request* request) {
    IP_t    ip          = WifiService::get_ip();
    char    ip_text[16] = {};
    char    sta_mac[18] = {};
    char    ap_mac[18]  = {};
    uint8_t channel     = 0;
    ip_to_str(ip, ip_text, sizeof(ip_text));
    mac_to_str(WiFiManager::instance().get_mac(WIFI_IF_STA), sta_mac, sizeof(sta_mac));
    mac_to_str(WiFiManager::instance().get_mac(WIFI_IF_AP), ap_mac, sizeof(ap_mac));
    auto                       cfg       = WifiService::get_config();
    EspNowLink::LinkStatistics now_stats = {};
    EspNowLink::get_statistics(&now_stats);
    char   peer_json[192]      = {};
    size_t peer_json_pos       = 0;
    peer_json[peer_json_pos++] = '[';
    const size_t peer_count    = EspNowLink::get_saved_peer_count();
    for (size_t i = 0; i < peer_count && i < 3; ++i) {
        EspNowLink::SavedPeer peer = {};
        if (EspNowLink::get_saved_peer(i, &peer) != ESP_OK) {
            continue;
        }
        const int written =
            snprintf(peer_json + peer_json_pos, sizeof(peer_json) - peer_json_pos,
                     "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"channel\":%" PRIu32 "}",
                     peer_json_pos == 1 ? "" : ",",
                     peer.address.bytes[0], peer.address.bytes[1], peer.address.bytes[2], peer.address.bytes[3],
                     peer.address.bytes[4], peer.address.bytes[5], static_cast<uint32_t>(peer.last_channel));
        if (written < 0 || static_cast<size_t>(written) >= sizeof(peer_json) - peer_json_pos) {
            break;
        }
        peer_json_pos += static_cast<size_t>(written);
    }
    snprintf(peer_json + peer_json_pos, sizeof(peer_json) - peer_json_pos, "]");
    const bool channel_available = WifiService::get_channel(&channel) == ESP_OK;
    snprintf(
        response_buffer, sizeof(response_buffer),
        "{\"mode\":\"%s\",\"state\":%d,\"ip\":\"%s\",\"saved_ssid\":\"%s\",\"ap_ssid\":\"%s\",\"rssi\":%d,\"signal_"
        "percent\":%" PRIu32 ",\"channel\":%" PRIu32
        ",\"channel_available\":%s,\"sta_mac\":\"%s\",\"ap_mac\":\"%s\",\"boot_enabled\":%s,"
        "\"espnow_active\":%s,\"espnow_pairing\":%s,\"espnow_peer_count\":%" PRIu32
        ",\"espnow_peers\":%s,\"espnow_tx\":%" PRIu32 ","
        "\"espnow_no_ack\":%" PRIu32 ",\"espnow_invalid\":%" PRIu32 ",\"espnow_timing\":%" PRIu32
        ",\"last_error\":\"%s\"}\n",
        mode_to_str(WifiService::get_mode()), (int)WifiService::get_wifi_state(), ip_text, cfg.ssid,
        WifiService::get_ap_ssid(), static_cast<int>(WifiService::get_rssi()),
        static_cast<uint32_t>(WifiService::get_signal_percent()), static_cast<uint32_t>(channel),
        channel_available ? "true" : "false", sta_mac, ap_mac, cfg.web_enabled_on_boot ? "true" : "false",
        EspNowLink::is_active() ? "true" : "false", EspNowLink::is_pairing() ? "true" : "false",
        static_cast<uint32_t>(peer_count), peer_json, static_cast<uint32_t>(now_stats.tx_packets),
        static_cast<uint32_t>(now_stats.ack_timeouts), static_cast<uint32_t>(now_stats.rx_invalid_packets),
        static_cast<uint32_t>(now_stats.timing_errors), WifiService::get_last_error());
    return WebServer::send_json(request, response_buffer);
}

/** @brief 将 ESP-IDF WiFi 鉴权枚举转换为前端显示字符串。 */
static const char* authmode_to_str(wifi_auth_mode_t authmode) {
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa_wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2_enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2_wpa3";
    case WIFI_AUTH_WAPI_PSK:
        return "wapi";
    default:
        return "unknown";
    }
}

/**
 * @brief 将文本追加为 JSON 字符串内容。
 *
 * 这里不负责写入开闭引号，只处理引号、反斜杠和控制字符转义。
 * 扫描到的 SSID 和日志内容都可能包含需要转义的字符。
 */
static size_t append_json_escaped(char* out, size_t out_size, size_t pos, const char* text) {
    if (pos >= out_size) {
        return pos;
    }
    for (const char* p = text; *p != '\0' && pos < out_size - 1; ++p) {
        if (*p == '"' || *p == '\\') {
            if (pos + 2 >= out_size) {
                break;
            }
            out[pos++] = '\\';
            out[pos++] = *p;
        } else if (static_cast<uint8_t>(*p) < 0x20) {
            if (pos + 6 >= out_size) {
                break;
            }
            int n = snprintf(out + pos, out_size - pos, "\\u%04x", static_cast<uint8_t>(*p));
            if (n < 0) {
                break;
            }
            pos += static_cast<size_t>(n);
        } else {
            out[pos++] = *p;
        }
    }
    out[pos] = '\0';
    return pos;
}

/**
 * @brief 带边界检查的 snprintf 追加工具。
 *
 * 由于响应使用固定静态缓冲，所有拼接都必须检查剩余空间。
 * 返回 false 时调用方应停止拼接并返回 response_too_large。
 */
static bool append_checked(char* out, size_t out_size, size_t* pos, const char* fmt, ...) {
    if (out == nullptr || pos == nullptr || *pos >= out_size) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + *pos, out_size - *pos, fmt, args);
    va_end(args);

    if (n < 0 || static_cast<size_t>(n) >= out_size - *pos) {
        if (out_size > 0) {
            out[out_size - 1] = '\0';
        }
        return false;
    }
    *pos += static_cast<size_t>(n);
    return true;
}

/** @brief GET /api/logs，按 since 参数增量读取 Web 实时日志。 */
esp_err_t logs_api_handler(WebServer::Request* request) {
    char     since_text[24] = {};
    uint64_t since          = 0;
    if (WebServer::get_query_value(request, "since", since_text, sizeof(since_text)) == ESP_OK) {
        since = strtoull(since_text, nullptr, 10);
    }

    uint64_t from_seq   = 0;
    uint64_t next_seq   = 0;
    uint64_t latest_seq = 0;
    bool     dropped    = false;
    size_t   len        = read_log_ring(since, log_snapshot_buffer, sizeof(log_snapshot_buffer), &from_seq, &next_seq,
                                        &latest_seq, &dropped);

    size_t pos = 0;
    bool   ok  = append_checked(detail_response_buffer, sizeof(detail_response_buffer), &pos,
                                "{\"from\":%" PRIu64 ",\"seq\":%" PRIu64 ",\"latest\":%" PRIu64
                                ",\"dropped\":%s,\"bytes\":%" PRIu32 ",\"text\":\"",
                                from_seq, next_seq, latest_seq, dropped ? "true" : "false", static_cast<uint32_t>(len));
    if (ok) {
        pos = append_json_escaped(detail_response_buffer, sizeof(detail_response_buffer), pos, log_snapshot_buffer);
        ok  = append_checked(detail_response_buffer, sizeof(detail_response_buffer), &pos, "\"}\n");
    }
    if (!ok) {
        snprintf(detail_response_buffer, sizeof(detail_response_buffer),
                 "{\"error\":\"response_too_large\",\"seq\":%" PRIu64 "}\n", next_seq);
    }
    return WebServer::send_json(request, detail_response_buffer);
}

/** @brief POST /api/logs/clear，清空 Web 实时日志缓冲区。 */
esp_err_t logs_clear_handler(WebServer::Request* request) {
    clear_log_ring();
    return WebServer::send_json(request, "{\"ok\":true,\"seq\":0}\n");
}

/** @brief GET /api/wifi/scan，扫描附近 AP 并返回有限数量结果。 */
esp_err_t wifi_scan_handler(WebServer::Request* request) {
    WifiService::ScanResult results[WifiService::WIFI_SCAN_MAX_RESULTS] = {};
    size_t                  count                                       = 0;
    esp_err_t               ret = WifiService::scan_ap_list(results, WifiService::WIFI_SCAN_MAX_RESULTS, &count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: reason=%s", esp_err_to_name(ret));
        snprintf(scan_response_buffer, sizeof(scan_response_buffer), "{\"ok\":false,\"reason\":\"%s\",\"aps\":[]}\n",
                 esp_err_to_name(ret));
        return WebServer::send_json(request, scan_response_buffer);
    }

    size_t pos = 0;
    bool   ok  = append_checked(scan_response_buffer, sizeof(scan_response_buffer), &pos,
                                "{\"ok\":true,\"count\":%" PRIu32 ",\"aps\":[", static_cast<uint32_t>(count));

    for (size_t i = 0; ok && i < count && pos < sizeof(scan_response_buffer) - 1; ++i) {
        ok = append_checked(scan_response_buffer, sizeof(scan_response_buffer), &pos, "%s{\"ssid\":\"",
                            i == 0 ? "" : ",");
        if (!ok) {
            break;
        }
        pos = append_json_escaped(scan_response_buffer, sizeof(scan_response_buffer), pos, results[i].ssid);
        ok  = append_checked(scan_response_buffer, sizeof(scan_response_buffer), &pos,
                             "\",\"rssi\":%d,\"channel\":%" PRIu32 ",\"auth\":\"%s\",\"secure\":%s}", results[i].rssi,
                             static_cast<uint32_t>(results[i].channel), authmode_to_str(results[i].authmode),
                            results[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
    }

    if (!ok || !append_checked(scan_response_buffer, sizeof(scan_response_buffer), &pos, "]}\n")) {
        snprintf(scan_response_buffer, sizeof(scan_response_buffer),
                 "{\"ok\":false,\"reason\":\"response_too_large\",\"aps\":[]}\n");
    }
    return WebServer::send_json(request, scan_response_buffer);
}

/**
 * @brief POST /api/wifi/connect
 *
 * 请求体携带 SSID/password。连接成功后保存到 NVS；失败时回到 AP 配网模式。
 */
esp_err_t wifi_connect_handler(WebServer::Request* request) {
    esp_err_t ret = WebServer::load_body(request);
    if (ret != ESP_OK) {
        return ret;
    }

    char ssid[WIFI_SSID_MAX_LEN + 1]         = {};
    char password[WIFI_PASSWORD_MAX_LEN + 1] = {};
    if (!json_get_string(request->body, "ssid", ssid, sizeof(ssid))) {
        ESP_LOGW(TAG, "WiFi connect rejected: missing SSID");
        return WebServer::send(request, 400, "application/json", "{\"ok\":false,\"reason\":\"missing_ssid\"}\n",
                               strlen("{\"ok\":false,\"reason\":\"missing_ssid\"}\n"));
    }
    json_get_string(request->body, "password", password, sizeof(password));

    ret = WifiService::connect_sta(ssid, password, true, TAG);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed: ssid=%s reason=%s, restoring provision AP", ssid, esp_err_to_name(ret));
        WifiService::start_provision_ap(TAG);
    } else {
        ESP_LOGI(TAG, "WiFi connected: ssid=%s", ssid);
    }

    IP_t ip          = WifiService::get_ip();
    char ip_text[16] = {};
    ip_to_str(ip, ip_text, sizeof(ip_text));
    snprintf(response_buffer, sizeof(response_buffer), "{\"ok\":%s,\"reason\":\"%s\",\"ip\":\"%s\",\"mode\":\"%s\"}\n",
             ret == ESP_OK ? "true" : "false", ret == ESP_OK ? "ok" : esp_err_to_name(ret), ip_text,
             mode_to_str(WifiService::get_mode()));
    return WebServer::send_json(request, response_buffer);
}

/** @brief POST /api/wifi/ap，手动切换到 AP 配网模式。 */
esp_err_t wifi_ap_handler(WebServer::Request* request) {
    esp_err_t ret = WifiService::start_provision_ap(TAG);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi provision AP started: ssid=%s", WifiService::get_ap_ssid());
    } else {
        ESP_LOGW(TAG, "WiFi provision AP start failed: reason=%s", esp_err_to_name(ret));
    }
    IP_t ip          = WifiService::get_ip();
    char ip_text[16] = {};
    ip_to_str(ip, ip_text, sizeof(ip_text));
    snprintf(response_buffer, sizeof(response_buffer),
             "{\"ok\":%s,\"reason\":\"%s\",\"ap_ssid\":\"%s\",\"ip\":\"%s\"}\n", ret == ESP_OK ? "true" : "false",
             ret == ESP_OK ? "ok" : esp_err_to_name(ret), WifiService::get_ap_ssid(), ip_text);
    return WebServer::send_json(request, response_buffer);
}

/** @brief POST /api/wifi/off，关闭 IP 网络并切换到 ESPNOW_ONLY。 */
esp_err_t wifi_off_handler(WebServer::Request* request) {
    const BaseType_t task_result = xTaskCreate(wifi_off_deferred_task, "wifi_web_off", 3072, nullptr, 3, nullptr);
    const bool       scheduled   = task_result == pdPASS;
    snprintf(response_buffer, sizeof(response_buffer),
             "{\"ok\":%s,\"reason\":\"%s\",\"target_mode\":\"espnow_only\",\"espnow_active\":%s}\n",
             scheduled ? "true" : "false", scheduled ? "scheduled" : "no_memory",
             EspNowLink::is_active() ? "true" : "false");
    return WebServer::send_json(request, response_buffer);
}

/** @brief POST /api/wifi/on，按 NVS 配置启动默认 WiFi/Web 网络模式。 */
esp_err_t wifi_on_handler(WebServer::Request* request) {
    esp_err_t ret = WifiService::start_default(TAG);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi service started: mode=%s", mode_to_str(WifiService::get_mode()));
    } else {
        ESP_LOGW(TAG, "WiFi service start failed: reason=%s", esp_err_to_name(ret));
    }
    IP_t ip          = WifiService::get_ip();
    char ip_text[16] = {};
    ip_to_str(ip, ip_text, sizeof(ip_text));
    snprintf(response_buffer, sizeof(response_buffer), "{\"ok\":%s,\"reason\":\"%s\",\"mode\":\"%s\",\"ip\":\"%s\"}\n",
             ret == ESP_OK ? "true" : "false", ret == ESP_OK ? "ok" : esp_err_to_name(ret),
             mode_to_str(WifiService::get_mode()), ip_text);
    return WebServer::send_json(request, response_buffer);
}

/** @brief POST /api/wifi/boot，设置启动时是否自动启用 WiFi/Web。 */
esp_err_t wifi_boot_handler(WebServer::Request* request) {
    esp_err_t ret = WebServer::load_body(request);
    if (ret != ESP_OK) {
        return ret;
    }
    bool enabled = true;
    if (!json_get_bool(request->body, "enabled", &enabled)) {
        ESP_LOGW(TAG, "WiFi boot config rejected: missing enabled");
        return WebServer::send(request, 400, "application/json", "{\"ok\":false,\"reason\":\"missing_enabled\"}\n",
                               strlen("{\"ok\":false,\"reason\":\"missing_enabled\"}\n"));
    }
    ret = WifiService::set_web_enabled_on_boot(enabled, TAG);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi boot config updated: enabled=%s", enabled ? "true" : "false");
    } else {
        ESP_LOGW(TAG, "WiFi boot config update failed: reason=%s", esp_err_to_name(ret));
    }
    snprintf(response_buffer, sizeof(response_buffer), "{\"ok\":%s,\"reason\":\"%s\",\"boot_enabled\":%s}\n",
             ret == ESP_OK ? "true" : "false", ret == ESP_OK ? "ok" : esp_err_to_name(ret),
             WifiService::is_web_enabled_on_boot() ? "true" : "false");
    return WebServer::send_json(request, response_buffer);
}

/** @brief POST /api/wifi/clear，清除已保存的 STA 凭据。 */
esp_err_t wifi_clear_handler(WebServer::Request* request) {
    esp_err_t ret = WifiService::clear_saved_sta(TAG);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "saved WiFi credentials cleared");
    } else {
        ESP_LOGW(TAG, "clear saved WiFi credentials failed: reason=%s", esp_err_to_name(ret));
    }
    snprintf(response_buffer, sizeof(response_buffer), "{\"ok\":%s,\"reason\":\"%s\"}\n",
             ret == ESP_OK ? "true" : "false", ret == ESP_OK ? "ok" : esp_err_to_name(ret));
    return WebServer::send_json(request, response_buffer);
}

/** @brief POST /api/espnow/pair，持续等待一个设备配对，成功或手动退出后关闭。 */
esp_err_t espnow_pair_handler(WebServer::Request* request) {
    esp_err_t ret = WebServer::load_body(request);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = EspNowLink::enter_pairing_mode(0);
    if (ret == ESP_OK) {
        DEVICE_EVENT_I(TAG, "espnow: pairing source=web action=start unlimited=1 result=ok");
    } else {
        ESP_LOGW(TAG, "ESP-NOW pairing start failed: reason=%s", esp_err_to_name(ret));
    }
    snprintf(
        response_buffer, sizeof(response_buffer),
        "{\"ok\":%s,\"reason\":\"%s\",\"pairing\":%s,\"peer_count\":%" PRIu32
        ",\"single_device\":true,\"unlimited\":true}\n",
        ret == ESP_OK ? "true" : "false", ret == ESP_OK ? "ok" : esp_err_to_name(ret),
        EspNowLink::is_pairing() ? "true" : "false", static_cast<uint32_t>(EspNowLink::get_saved_peer_count()));
    return WebServer::send_json(request, response_buffer);
}

/** @brief POST /api/espnow/pair/stop，手动关闭当前配对窗口。 */
esp_err_t espnow_pair_stop_handler(WebServer::Request* request) {
    EspNowLink::leave_pairing_mode();
    DEVICE_EVENT_I(TAG, "espnow: pairing source=web action=stop result=ok");
    snprintf(response_buffer, sizeof(response_buffer),
             "{\"ok\":true,\"pairing\":false,\"peer_count\":%" PRIu32 "}\n",
             static_cast<uint32_t>(EspNowLink::get_saved_peer_count()));
    return WebServer::send_json(request, response_buffer);
}

/** @brief POST /api/espnow/pair/clear，退出配对并清除全部已保存 peer。 */
esp_err_t espnow_pair_clear_handler(WebServer::Request* request) {
    EspNowLink::leave_pairing_mode();
    const esp_err_t ret = EspNowLink::clear_saved_peers();
    if (ret == ESP_OK) {
        DEVICE_EVENT_I(TAG, "espnow: peers source=web action=clear result=ok");
    } else {
        ESP_LOGW(TAG, "clear ESP-NOW paired peers failed: reason=%s", esp_err_to_name(ret));
    }
    snprintf(response_buffer, sizeof(response_buffer),
             "{\"ok\":%s,\"reason\":\"%s\",\"pairing\":false,\"peer_count\":%" PRIu32 "}\n",
             ret == ESP_OK ? "true" : "false",
             ret == ESP_OK ? "ok" : esp_err_to_name(ret), static_cast<uint32_t>(EspNowLink::get_saved_peer_count()));
    return WebServer::send_json(request, response_buffer);
}

} // namespace WebBackend

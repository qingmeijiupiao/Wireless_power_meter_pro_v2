#include "shell_command.h"
#include "shell.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hardware.h"
#include "st7789.h"
#include "can_callback.h"
#include "blackbox.h"
#include "blackbox_service.h"
#include "diagnostic_log.h"
#include "current_calibration.h"
#include "energy_meter.h"
#include "global_state.h"
#include "HXC_NVS.h"
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "power_output.h"
#include "protect.h"
#include "screen.h"
#include "short_circuit_detect.h"
#include "wifi_service.h"
#include "espnow_link.h"
#include "espnow_service.h"
#include "web_backend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace ShellCommand {

static constexpr char TAG[] = "ShellCommand";

static void print_escaped_text(const char* text) {
    putchar('"');
    for (const uint8_t* cursor = reinterpret_cast<const uint8_t*>(text); *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '\\':
            printf("\\\\");
            break;
        case '"':
            printf("\\\"");
            break;
        case '\r':
            printf("\\r");
            break;
        case '\n':
            printf("\\n");
            break;
        case '\t':
            printf("\\t");
            break;
        default:
            if (std::isprint(static_cast<uint8_t>(*cursor))) {
                putchar(*cursor);
            } else {
                printf("\\x%02X", *cursor);
            }
            break;
        }
    }
    putchar('"');
}

static void print_hex(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        printf("%02X", data[i]);
    }
}

static bool parse_float_arg(const char* text, float* out) {
    if (text == nullptr || out == nullptr || text[0] == '\0') {
        return false;
    }
    char*       end   = nullptr;
    const float value = strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

// ====== 命令列表 ======

esp_err_t init() {
    auto&     shell = Shell::instance();
    esp_err_t ret   = shell.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "shell init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /**
     * @brief  reboot - 重启设备
     * @usage  reboot
     * @note   无参数，调用后设备立即重启
     */
    shell.register_command(ShellCommand_t("reboot", "Reboot the device", "", [](int argc, char** argv) -> int {
        DEVICE_STATE_W(TAG, "system: reboot source=shell delay_ms=0");
        printf("Rebooting...\n");
        esp_restart();
        return 0;
    }));

    /**
     * @brief  timestamp - 获取系统时间戳
     * @usage  timestamp
     * @note   输出自上电以来的微秒数，基于 esp_timer
     */
    shell.register_command(
        ShellCommand_t("timestamp", "Get system timestamp since boot (us)", "", [](int argc, char** argv) -> int {
            printf("%" PRId64 " us\n", static_cast<int64_t>(esp_timer_get_time()));
            return 0;
        }));

    shell.register_command(ShellCommand_t(
        "rtos_stats", "Sample per-task CPU usage and stack high-water marks", "[seconds]",
        [](int argc, char** argv) -> int {
            int sample_seconds = argc >= 2 ? atoi(argv[1]) : 10;
            if (sample_seconds < 1 || sample_seconds > 300) {
                printf("Usage: rtos_stats [seconds: 1-300]\n");
                return 1;
            }

            auto snapshot = []() {
                std::vector<TaskStatus_t>   tasks(uxTaskGetNumberOfTasks() + 4);
                configRUN_TIME_COUNTER_TYPE total_runtime = 0;
                UBaseType_t                 count = uxTaskGetSystemState(tasks.data(), tasks.size(), &total_runtime);
                tasks.resize(count);
                return std::make_pair(std::move(tasks), total_runtime);
            };

            auto before = snapshot();
            printf("Sampling RTOS statistics for %d second(s)...\n", sample_seconds);
            vTaskDelay(pdMS_TO_TICKS(sample_seconds * 1000));
            auto                              after       = snapshot();
            const configRUN_TIME_COUNTER_TYPE total_delta = after.second - before.second;

            printf("RTOS_STATS_BEGIN sample_s=%d tasks=%" PRIu32 " total_delta=%" PRIu64 "\n", sample_seconds,
                   static_cast<uint32_t>(after.first.size()), static_cast<uint64_t>(total_delta));
            printf("%-16s %5s %4s %9s %14s %14s %14s\n", "TASK", "STATE", "PRIO", "CPU(%)", "RUNTIME_DELTA",
                   "RUNTIME_TOTAL", "STACK_FREE_MIN");
            printf("---------------- ----- ---- --------- -------------- -------------- --------------\n");
            for (const auto& task : after.first) {
                auto previous =
                    std::find_if(before.first.begin(), before.first.end(),
                                 [&task](const TaskStatus_t& item) { return item.xTaskNumber == task.xTaskNumber; });
                const configRUN_TIME_COUNTER_TYPE runtime_delta =
                    previous == before.first.end() ? 0 : task.ulRunTimeCounter - previous->ulRunTimeCounter;
                const double cpu_pct =
                    total_delta == 0 ? 0.0
                                     : 100.0 * static_cast<double>(runtime_delta) / static_cast<double>(total_delta);
                printf("%-16s %5d %4" PRIu32 " %9.3f %14" PRIu64 " %14" PRIu64 " %14" PRIu32 "\n",
                       task.pcTaskName,
                       static_cast<int>(task.eCurrentState), static_cast<uint32_t>(task.uxCurrentPriority), cpu_pct,
                       static_cast<uint64_t>(runtime_delta),
                       static_cast<uint64_t>(task.ulRunTimeCounter),
                       static_cast<uint32_t>(task.usStackHighWaterMark));
            }
            printf("RTOS_STATS_END\n");
            return 0;
        }));

    /**
     * @brief  version - 获取固件版本号与编译时间
     * @usage  version
     * @note   输出版本号、编译时间、硬件版本；本地固件打印 WARNING
     */
    shell.register_command(
        ShellCommand_t("version", "Get firmware version and build time", "", [](int argc, char** argv) -> int {
            printf("Hardware: v%d\n", get_hardware_version());
            if (VERSION_PATCH == 99) {
                printf("Firmware: %d.%d.%d (Local build not official firmware!)\n", VERSION_MAJOR, VERSION_MINOR,
                       VERSION_PATCH);
            } else {
                printf("Firmware: %d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
            }
            // BUILD_TIME is generated by CMake in fixed UTC+8 time, so local and CD builds display consistently.
            printf("Build:    %s\n", BUILD_TIME);
            return 0;
        }));

    /**
     * @brief  espnow - ESP-NOW 配对和 peer 管理
     * @usage  espnow [status|pair [timeout_s]|stop|clear]
     * @param  pair - 开启一次配对窗口，成功配对一个设备后自动退出
     * @param  stop - 手动退出当前配对窗口
     * @param  clear - 退出配对并清除全部已保存 peer
     */
    shell.register_command(ShellCommand_t(
        "espnow", "ESP-NOW pairing and peer control", "status|pair [timeout_s]|stop|clear",
        [](int argc, char** argv) -> int {
            auto print_status = []() {
                uint8_t    channel           = 0;
                const bool channel_available = WifiService::get_channel(&channel) == ESP_OK;
                printf("active: %d, pairing: %d, paired_peers: %" PRIu32 ", channel: %s%" PRIu32 "\n",
                       EspNowLink::is_active() ? 1 : 0,
                       EspNowLink::is_pairing() ? 1 : 0, static_cast<uint32_t>(EspNowLink::get_saved_peer_count()),
                       channel_available ? "" : "N/A", channel_available ? static_cast<uint32_t>(channel) : 0U);
            };

            if (argc < 2 || strcmp(argv[1], "status") == 0) {
                print_status();
                return 0;
            }

            if (strcmp(argv[1], "pair") == 0) {
                uint32_t timeout_s = 60;
                if (argc >= 3) {
                    char*               end    = nullptr;
                    const uint32_t parsed = strtoul(argv[2], &end, 10);
                    if (end == argv[2] || *end != '\0' || parsed < 1 || parsed > 300) {
                        printf("Usage: espnow pair [timeout_s], timeout range 1-300\n");
                        return 1;
                    }
                    timeout_s = static_cast<uint32_t>(parsed);
                }
                const esp_err_t ret = EspNowLink::enter_pairing_mode(timeout_s * 1000);
                printf("single-device pairing start: %s, timeout_s: %" PRIu32 "\n", esp_err_to_name(ret),
                       static_cast<uint32_t>(timeout_s));
                print_status();
                return ret == ESP_OK ? 0 : 1;
            }

            if (strcmp(argv[1], "stop") == 0) {
                EspNowLink::leave_pairing_mode();
                printf("pairing stopped\n");
                print_status();
                return 0;
            }

            if (strcmp(argv[1], "clear") == 0) {
                EspNowLink::leave_pairing_mode();
                const esp_err_t ret = EspNowLink::clear_saved_peers();
                printf("paired peers clear: %s\n", esp_err_to_name(ret));
                print_status();
                return ret == ESP_OK ? 0 : 1;
            }

            printf("Usage: espnow status|pair [timeout_s]|stop|clear\n");
            return 1;
        }));

    /**
     * @brief  backlight - 设置屏幕背光亮度
     * @usage  backlight <0-255>
     * @param  brightness - 亮度值，0为最暗，255为最亮
     * @note   不带参数时显示当前亮度
     */
    shell.register_command(ShellCommand_t("backlight", "Set screen backlight brightness (0-255)", "<brightness>",
                                          [](int argc, char** argv) -> int {
                                              if (argc < 2) {
                                                  // 不带参数时显示当前亮度
                                                  uint8_t current_brightness = ST7789::get_backlight();
                                                  printf("Current backlight(0-255): %d\n", current_brightness);
                                                  return 0;
                                              }
                                              int brightness = atoi(argv[1]);
                                              if (brightness < 0 || brightness > 255) {
                                                  printf("Error: brightness must be 0-255\n");
                                                  return 1;
                                              }
                                              esp_err_t ret = ST7789::set_backlight((uint8_t)brightness);
                                              if (ret != ESP_OK) {
                                                  printf("Failed to set backlight\n");
                                                  return 1;
                                              }
                                              DEVICE_EVENT_I(TAG, "ui: config source=shell backlight=%d", brightness);
                                              printf("Backlight set to %d\n", brightness);
                                              return 0;
                                          }));

    /**
     * @brief  can_baudrate - 设置CAN波特率
     * @usage  can_baudrate [baudrate]
     * @param  baudrate - 波特率值(10进制)，如 1000000
     * @note   不带参数时显示当前波特率
     */
    shell.register_command(
        ShellCommand_t("can_baudrate", "Set CAN baudrate (decimal)", "<baudrate>", [](int argc, char** argv) -> int {
            if (argc < 2) {
                printf("Current CAN baudrate: %" PRIu32 "\n", static_cast<uint32_t>(CanCallback::CAN_BAUDRATE));
                return 0;
            }
            uint32_t baudrate = static_cast<uint32_t>(strtoul(argv[1], nullptr, 10));
            if (baudrate == 0) {
                printf("Error: invalid baudrate\n");
                return 1;
            }
            const esp_err_t err = CanCallback::CAN_BAUDRATE.set(baudrate);
            if (err != ESP_OK) {
                printf("Error: failed to persist CAN baudrate: %s\n", esp_err_to_name(err));
                return 1;
            }
            DEVICE_EVENT_I(TAG, "can: config baud=%" PRIu32 " source=shell reboot_required=1",
                           static_cast<uint32_t>(baudrate));
            printf("CAN baudrate set to %" PRIu32 "\n", baudrate);
            return 0;
        }));

    /**
     * @brief  can_id - 设置CAN ID
     * @usage  can_id [id]
     * @param  id - CAN ID，支持10进制和16进制(0x前缀)，如 0x400 或 1024
     * @note   不带参数时显示当前CAN ID(同时显示10进制和16进制)
     */
    shell.register_command(
        ShellCommand_t("can_id", "Set CAN ID (decimal or 0x hex)", "<id>", [](int argc, char** argv) -> int {
            if (argc < 2) {
                uint32_t id = CanCallback::CAN_ID;
                printf("Current CAN ID: %" PRIu32 " (0x%" PRIX32 ")\n", id, id);
                return 0;
            }
            uint32_t        id  = static_cast<uint32_t>(strtoul(argv[1], nullptr, 0));
            const esp_err_t err = CanCallback::CAN_ID.set(id);
            if (err != ESP_OK) {
                printf("Error: failed to persist CAN ID: %s\n", esp_err_to_name(err));
                return 1;
            }
            DEVICE_EVENT_I(TAG, "can: config id=0x%" PRIx32 " source=shell reboot_required=1", static_cast<uint32_t>(id));
            printf("CAN ID set to %" PRIu32 " (0x%" PRIX32 ")\n", id, id);
            return 0;
        }));

    /**
     * @brief  get_data - 获取当前数据
     * @usage  get_data
     * @note   显示当前电压V,电流A,温度°C
     */
    shell.register_command(ShellCommand_t(
        "get_data", "Get current data (voltage, current, temperature)", "", [](int argc, char** argv) -> int {
            auto  state       = get_global_state();
            float voltage     = (float)state.voltage_mV / 1e3;
            float current     = (float)state.current_uA / 1e6;
            float temperature = (float)state.board_temperature / 100.0;
            printf("voltage: %.3f V, current: %.3f A, temperature: %.2f C\n", voltage, current, temperature);
            return 0;
        }));

    shell.register_command(ShellCommand_t(
        "meter", "Get or reset shared energy meter session", "[status|reset]", [](int argc, char** argv) -> int {
            if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
                EnergyMeter::reset();
                DEVICE_EVENT_I(TAG, "meter: reset source=shell");
                printf("Shared meter session reset\n");
            } else if (argc >= 2 && strcmp(argv[1], "status") != 0) {
                printf("Usage: meter [status|reset]\n");
                return 1;
            }

            const auto     meter          = EnergyMeter::snapshot();
            const auto     state          = get_global_state();
            const uint64_t meter_seconds  = meter.meter_time_ms / 1000;
            const uint64_t system_time_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
            const uint64_t system_seconds = system_time_ms / 1000;
            const double   voltage_v      = state.voltage_mV / 1000.0;
            const double   current_a      = state.current_uA / 1000000.0;

            printf("Shared meter session:\n");
            printf("  energy:       %.3f mWh (%" PRId64 " uWh)\n", meter.energy_uwh / 1000.0,
                   static_cast<int64_t>(meter.energy_uwh));
            printf("  charge:       %.3f mAh (%" PRId64 " uAh)\n", meter.charge_uah / 1000.0,
                   static_cast<int64_t>(meter.charge_uah));
            printf("  meter time:   %" PRIu64 " ms (%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ")\n",
                   static_cast<uint64_t>(meter.meter_time_ms),
                   static_cast<uint64_t>(meter_seconds / 3600),
                   static_cast<uint64_t>((meter_seconds / 60) % 60),
                   static_cast<uint64_t>(meter_seconds % 60));
            printf("LP Core lifetime counters:\n");
            const EnergyMeter::Snapshot lifetime = EnergyMeter::lifetime_snapshot();
            printf("  energy:       %" PRId64 " uWh\n", static_cast<int64_t>(lifetime.energy_uwh));
            printf("  charge:       %" PRId64 " uAh\n", static_cast<int64_t>(lifetime.charge_uah));
            printf("System:\n");
            printf("  uptime:       %" PRIu64 " ms (%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ")\n",
                   static_cast<uint64_t>(system_time_ms),
                   static_cast<uint64_t>(system_seconds / 3600),
                   static_cast<uint64_t>((system_seconds / 60) % 60),
                   static_cast<uint64_t>(system_seconds % 60));
            printf("  voltage:      %.3f V\n", voltage_v);
            printf("  current:      %.6f A\n", current_a);
            printf("  power:        %.3f W\n", voltage_v * current_a);
            return 0;
        }));

    /**
     * @brief  start_logo - set startup logo display duration
     * @usage  start_logo [duration_ms]
     * @note   Zero disables the startup logo. Changes take effect after reboot.
     */
    shell.register_command(ShellCommand_t(
        "start_logo", "Set startup logo duration in milliseconds (0 disables)", "[duration_ms]",
        [](int argc, char** argv) -> int {
            if (argc < 2) {
                printf("Startup logo duration: %" PRIu32 " ms\n",
                       static_cast<uint32_t>(SCREEN::get_start_logo_duration_ms()));
                return 0;
            }

            char*         end         = nullptr;
            uint32_t duration_ms = static_cast<uint32_t>(strtoul(argv[1], &end, 10));
            if (argv[1][0] == '\0' || *end != '\0' || duration_ms > SCREEN::MAX_START_LOGO_DURATION_MS) {
                printf("Usage: start_logo [duration_ms: 0-%" PRIu32 "]\n",
                       static_cast<uint32_t>(SCREEN::MAX_START_LOGO_DURATION_MS));
                return 1;
            }

            const esp_err_t err = SCREEN::set_start_logo_duration_ms(static_cast<uint32_t>(duration_ms));
            if (err != ESP_OK) {
                printf("Error: failed to persist startup logo duration: %s\n", esp_err_to_name(err));
                return 1;
            }
            DEVICE_EVENT_I(TAG, "ui: config source=shell start_logo_ms=%" PRIu32 " reboot_required=1", duration_ms);
            printf("Startup logo duration set to %" PRIu32 " ms, restart required\n", duration_ms);
            return 0;
        }));

    /**
     * @brief  blackbox - 黑匣子日志管理
     * @usage  blackbox [status|dump [count|all]|pull [count|all]|clear|mark <text>]
     * @param  status - 查看启用状态和已落盘原始记录数
     * @param  dump/pull - 同步在途记录后，按从新到旧顺序拉取日志；默认 100 条
     * @param  clear - 同步清空分区；成功后保留一条 reset 标记
     */
    shell.register_command(ShellCommand_t(
        "blackbox", "Blackbox log control", "[status|dump [count|all]|pull [count|all]|clear|mark <text>]",
        [](int argc, char** argv) -> int {
            const char* action = argc >= 2 ? argv[1] : "status";

            if (strcmp(action, "status") == 0) {
                printf("Blackbox status: enabled=%d persisted_records=%" PRIu32 "\n", Blackbox::is_enabled(),
                       static_cast<uint32_t>(Blackbox::count()));
                return 0;
            }

            if (strcmp(action, "clear") == 0) {
                esp_err_t ret = Blackbox::erase_all();
                printf("Blackbox clear: %s, persisted_records=%" PRIu32 "\n", esp_err_to_name(ret),
                       static_cast<uint32_t>(Blackbox::count()));
                return ret == ESP_OK ? 0 : 1;
            }

            if (strcmp(action, "mark") == 0) {
                if (argc < 3) {
                    printf("Usage: blackbox mark <text>\n");
                    return 1;
                }
                char   text[96] = {};
                size_t pos      = 0;
                for (int i = 2; i < argc && pos < sizeof(text) - 1; ++i) {
                    if (i > 2 && pos < sizeof(text) - 1) {
                        text[pos++] = ' ';
                    }
                    for (const char* cursor = argv[i]; *cursor != '\0' && pos < sizeof(text) - 1; ++cursor) {
                        const char ch = *cursor;
                        text[pos++]   = (ch == '\r' || ch == '\n' || ch == '\t') ? ' ' : ch;
                    }
                }
                DEVICE_EVENT_I(TAG, "mark: source=shell text=%s", text);
                printf("Blackbox mark added: %s\n", text);
                return 0;
            }

            if (strcmp(action, "dump") == 0 || strcmp(action, "pull") == 0) {
                uint32_t    limit       = 100;
                const char* limit_label = "100";
                if (argc >= 3) {
                    if (strcmp(argv[2], "all") == 0) {
                        limit       = UINT32_MAX;
                        limit_label = "all";
                    } else {
                        char*         end    = nullptr;
                        uint32_t parsed = strtoul(argv[2], &end, 10);
                        if (argv[2][0] == '\0' || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
                            printf("Usage: blackbox %s [count|all]\n", action);
                            return 1;
                        }
                        limit       = static_cast<uint32_t>(parsed);
                        limit_label = argv[2];
                    }
                }
                if (argc >= 4) {
                    printf("Usage: blackbox %s [count|all]\n", action);
                    return 1;
                }

                esp_err_t ret = Blackbox::sync();
                if (ret != ESP_OK) {
                    printf("Blackbox sync failed: %s\n", esp_err_to_name(ret));
                    return 1;
                }

                const uint32_t raw_count = Blackbox::count();
                printf("BLACKBOX_DUMP_BEGIN persisted_records=%" PRIu32 " limit=%s order=newest_first\n",
                       static_cast<uint32_t>(raw_count), limit_label);
                uint32_t emitted = 0;
                uint32_t index   = 0;
                for (; index < raw_count && emitted < limit;) {
                    const Blackbox::Record record = Blackbox::read(index);
                    if (record.header.sof != CircularFlashBuffer::BLOCK_SOF) {
                        printf("record=%" PRIu32 " type=INVALID\n", static_cast<uint32_t>(index));
                        ++index;
                        ++emitted;
                        continue;
                    }

                    if (record.header.type == Blackbox::LogType::STRING) {
                        const Blackbox::TextRecord text = Blackbox::read_text(index);
                        if (text.record_count != 0) {
                            printf("record=%" PRIu32 " timestamp_ms=%" PRIu32
                                   " type=STRING fragments=%" PRIu32 " text=",
                                   static_cast<uint32_t>(index),
                                   static_cast<uint32_t>(record.header.timestamp),
                                   static_cast<uint32_t>(text.record_count));
                            print_escaped_text(text.str);
                            putchar('\n');
                            index += text.record_count;
                            ++emitted;
                            continue;
                        }
                    }

                    printf("record=%" PRIu32 " timestamp_ms=%" PRIu32 " type=%s payload=", static_cast<uint32_t>(index),
                           static_cast<uint32_t>(record.header.timestamp),
                           record.header.type == Blackbox::LogType::STRUCTURED ? "STRUCTURED" : "UNKNOWN");
                    print_hex(record.payload.bytes, Blackbox::PAYLOAD_SIZE);
                    putchar('\n');
                    ++index;
                    ++emitted;
                }
                printf("BLACKBOX_DUMP_END emitted=%" PRIu32 " consumed_records=%" PRIu32
                       " remaining_records=%" PRIu32 "\n",
                       static_cast<uint32_t>(emitted), static_cast<uint32_t>(index),
                       static_cast<uint32_t>(raw_count - index));
                return 0;
            }

            printf("Usage: blackbox [status|dump [count|all]|pull [count|all]|clear|mark <text>]\n");
            return 1;
        }));

    /**
     * @brief  output - 输出控制
     * @param  state - 输出状态，0为关闭，1为开启
     * @usage  output <state>
     * @note   设置输出状态，0为关闭，1为开启
     */
    shell.register_command(
        ShellCommand_t("output", "Set output state (0-1)", "<state>", [](int argc, char** argv) -> int {
            if (argc < 2) {
                printf("Now output state: %d\n", PowerOutput::get_state());
                return 1;
            }
            int state = atoi(argv[1]);
            if (state < 0 || state > 1) {
                printf("Error: state must be 0-1\n");
                return 1;
            }
            if (state == 0) {
                PowerOutput::off(TAG);
                printf("Output off\n");
            } else {
                PowerOutput::on(TAG);
                printf("Output on\n");
            }
            return 0;
        }));

    /**
     * @brief  short_detect - 输出端短路检测调试命令
     * @usage  short_detect [status|test|threshold [voltage_V]]
     * @note   当前仅用于手动测试，执行 test 前应确保主输出关闭。
     */
    shell.register_command(ShellCommand_t(
        "short_detect", "Short-circuit detector control", "status|test|threshold [voltage_V]",
        [](int argc, char** argv) -> int {
            if (argc == 1 || strcmp(argv[1], "status") == 0) {
                if (argc > 2) {
                    printf("Usage: short_detect status\n");
                    return 1;
                }
                const uint16_t threshold_mV = ShortCircuitDetect::get_threshold_mV();
                printf("Short detect threshold: %.3f V (%u mV)\n", threshold_mV / 1000.0f,
                       static_cast<unsigned>(threshold_mV));
                printf("Test pulse is idle; run 'short_detect test' with main output off\n");
                return 0;
            }

            if (strcmp(argv[1], "test") == 0) {
                if (argc != 2) {
                    printf("Usage: short_detect test\n");
                    return 1;
                }
                ShortCircuitDetect::Result result = {};
                const esp_err_t err = ShortCircuitDetect::test(result);
                if (err != ESP_OK) {
                    printf("Short detect test failed: %s\n", esp_err_to_name(err));
                    return 1;
                }
                printf("Short detect result: %s, voltage=%.3f V (%u mV), threshold=%.3f V (%u mV), samples=%u\n",
                       result.is_short ? "SHORT" : "OPEN", result.voltage_mV / 1000.0f,
                       static_cast<unsigned>(result.voltage_mV), result.threshold_mV / 1000.0f,
                       static_cast<unsigned>(result.threshold_mV), static_cast<unsigned>(result.sample_count));
                return 0;
            }

            if (strcmp(argv[1], "threshold") == 0) {
                if (argc == 2) {
                    const uint16_t threshold_mV = ShortCircuitDetect::get_threshold_mV();
                    printf("Short detect threshold: %.3f V (%u mV)\n", threshold_mV / 1000.0f,
                           static_cast<unsigned>(threshold_mV));
                    return 0;
                }
                if (argc != 3) {
                    printf("Usage: short_detect threshold <voltage_V>\n");
                    return 1;
                }

                float threshold_V = 0.0f;
                if (!parse_float_arg(argv[2], &threshold_V) || threshold_V < 0.001f || threshold_V > 3.3f) {
                    printf("Error: threshold must be 0.001-3.300 V\n");
                    return 1;
                }
                const uint16_t threshold_mV = static_cast<uint16_t>(std::lround(threshold_V * 1000.0f));
                const esp_err_t err = ShortCircuitDetect::set_threshold_mV(threshold_mV);
                if (err != ESP_OK) {
                    printf("Short detect threshold update failed: %s\n", esp_err_to_name(err));
                    return 1;
                }
                printf("Short detect threshold updated: %.3f V (%u mV)\n", threshold_mV / 1000.0f,
                       static_cast<unsigned>(threshold_mV));
                return 0;
            }

            printf("Usage: short_detect [status|test|threshold [voltage_V]]\n");
            return 1;
        }));

    /**
     * @brief  protect - 保护功能开关
     * @param  state - 保护状态，0为关闭保护，1为开启保护
     * @usage  protect [state]
     * @note   不带参数时显示当前保护开关状态和保护触发状态
     */
    shell.register_command(
        ShellCommand_t("protect", "Set protect state (0-1)", "<state>", [](int argc, char** argv) -> int {
            if (argc < 2) {
                auto state_to_str = [](ProtectState_t state) -> const char* {
                    switch (state) {
                    case PROTECT_STATE_NORMAL:
                        return "NORMAL";
                    case PROTECT_STATE_WARNING:
                        return "WARNING";
                    case PROTECT_STATE_PROTECT:
                        return "PROTECT";
                    default:
                        return "UNKNOWN";
                    }
                };
                printf("Protect state: %d, bypassed: %d, active fault: %d\n", !protect_is_bypassed(),
                       protect_is_bypassed(), protect_has_active_fault());
                printf("Name State   Now       Warn/Recover      Protect/Recover    Trigger\n");
                for (uint8_t i = 0; i < protect_get_channel_count(); i++) {
                    protect_channel_info_t info;
                    if (!protect_get_channel_info(i, &info)) {
                        continue;
                    }
                    printf("%-4s %-7s %7.3f%-2s %7.3f/%-7.3f %7.3f/%-7.3f %s\n", info.name, state_to_str(info.state),
                           info.now_value, info.unit, info.threshold.warning_threshold,
                           info.threshold.warning_recovery_threshold, info.threshold.protect_threshold,
                           info.threshold.protect_recovery_threshold, info.threshold.is_asc ? ">=" : "<=");
                }
                return 0;
            }
            int state = atoi(argv[1]);
            if (state < 0 || state > 1) {
                printf("Error: state must be 0-1\n");
                return 1;
            }
            protect_set_bypassed(state == 0, TAG);
            printf("Protect %s\n", state ? "on" : "off");
            if (state == 1 && protect_should_block_output()) {
                PowerOutput::off(TAG);
                printf("Active protect fault exists, output forced off\n");
            }
            return 0;
        }));

    /**
     * @brief  protect_threshold - 查询或设置保护阈值
     * @usage  protect_threshold [channel warning warning_recovery protect protect_recovery]
     * @param  channel - 通道编号：0=OTP，1=OVP，2=UVP，3=OCP
     * @note   参数单位与通道一致。修改立即生效并保存到 NVS。
     */
    shell.register_command(ShellCommand_t(
        "protect_threshold",
        "Get or set thresholds. channel: 0=OTP(C), 1=OVP(V), 2=UVP(V), 3=OCP(A). "
        "Order: OTP/OVP/OCP warning_recovery<=warning<=protect_recovery<=protect; "
        "UVP protect<=protect_recovery<=warning<=warning_recovery.",
        "[channel warning warning_recovery protect protect_recovery]", [](int argc, char** argv) -> int {
            if (argc == 1) {
                printf("Channel Unit Warning WarningRecovery Protect ProtectRecovery Trigger\n");
                for (uint8_t i = 0; i < protect_get_channel_count(); ++i) {
                    protect_channel_info_t info = {};
                    if (protect_get_channel_info(i, &info)) {
                        printf("%" PRIu32 ":%-4s %-4s %7.3f %15.3f %7.3f %15.3f %s\n",
                               static_cast<uint32_t>(i), info.name,
                               info.unit, info.threshold.warning_threshold, info.threshold.warning_recovery_threshold,
                               info.threshold.protect_threshold, info.threshold.protect_recovery_threshold,
                               info.threshold.is_asc ? ">=" : "<=");
                    }
                }
                return 0;
            }
            if (argc != 6) {
                printf(
                    "Usage: protect_threshold <channel> <warning> <warning_recovery> <protect> <protect_recovery>\n");
                return 1;
            }

            char*                  channel_end = nullptr;
            const int32_t          channel     = static_cast<int32_t>(strtol(argv[1], &channel_end, 10));
            protect_channel_info_t info        = {};
            if (channel_end == argv[1] || *channel_end != '\0' || channel < 0 ||
                channel >= protect_get_channel_count() ||
                !protect_get_channel_info(static_cast<uint8_t>(channel), &info)) {
                printf("Error: channel must be 0-3\n");
                return 1;
            }

            protect_threshold_t threshold        = info.threshold;
            float               warning          = 0.0f;
            float               warning_recovery = 0.0f;
            float               protect          = 0.0f;
            float               protect_recovery = 0.0f;
            if (!parse_float_arg(argv[2], &warning) || !parse_float_arg(argv[3], &warning_recovery) ||
                !parse_float_arg(argv[4], &protect) || !parse_float_arg(argv[5], &protect_recovery)) {
                printf("Error: thresholds must be finite numbers\n");
                return 1;
            }
            threshold.warning_threshold          = warning;
            threshold.warning_recovery_threshold = warning_recovery;
            threshold.protect_threshold          = protect;
            threshold.protect_recovery_threshold = protect_recovery;

            if (protect_set_channel_threshold(static_cast<uint8_t>(channel), threshold, TAG) != ESP_OK) {
                printf("Error: invalid thresholds; check non-negative values and recovery ordering\n");
                return 1;
            }
            printf("Protect threshold %s updated\n", info.name);
            return 0;
        }));

    /**
     * @brief  wifi - WiFi、Web 与 ESP-NOW 共用射频状态管理
     * @usage  wifi [status|ip|on|off|connect|ap|boot|clear]
     * @param  status/ip - 查询当前 WiFiService 模式、IP、已保存 SSID、配网 AP 名称和最近错误
     * @param  on - 按 NVS 配置启动 WiFi/Web，优先连接已保存 STA，失败则进入 AP 配网模式
     * @param  off - 关闭 STA/AP 与 Web，切换到 ESPNOW_ONLY，保留远程控制链路
     * @param  connect <ssid> [password] - 连接指定 WiFi，成功后保存到 NVS
     * @param  ap - 手动切换到 AP 配网模式
     * @param  boot <0|1> - 设置启动时是否默认启用 WiFi/Web 功能
     * @param  clear - 清除已保存的 STA SSID 和密码
     * @note   OFF 仅表示射频尚未启动；执行 wifi off 后进入 ESPNOW_ONLY，不会关闭 ESP-NOW。
     */
    shell.register_command(ShellCommand_t(
        "wifi", "WiFi/Web/ESP-NOW control", "status|ip|on|off|connect|ap|boot|clear", [](int argc, char** argv) -> int {
            auto mode_to_str = [](WifiService::Mode mode) -> const char* {
                switch (mode) {
                case WifiService::Mode::OFF:
                    return "OFF";
                case WifiService::Mode::ESPNOW_ONLY:
                    return "ESPNOW_ONLY";
                case WifiService::Mode::STA:
                    return "STA";
                case WifiService::Mode::AP_PROVISION:
                    return "AP_PROVISION";
                default:
                    return "UNKNOWN";
                }
            };

            auto print_status = [&]() {
                IP_t                       ip                = WifiService::get_ip();
                MAC_t                      sta_mac           = WiFiManager::instance().get_mac(WIFI_IF_STA);
                MAC_t                      ap_mac            = WiFiManager::instance().get_mac(WIFI_IF_AP);
                auto                       cfg               = WifiService::get_config();
                uint8_t                    channel           = 0;
                const bool                 channel_available = WifiService::get_channel(&channel) == ESP_OK;
                EspNowLink::LinkStatistics now_stats         = {};
                EspNowLink::get_statistics(&now_stats);
                printf("mode: %s, wifi_state: %d, web_running: %d, boot_enabled: %d\n",
                       mode_to_str(WifiService::get_mode()), (int)WifiService::get_wifi_state(),
                       WebBackend::is_running(), cfg.web_enabled_on_boot);
                printf("ip: %" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32
                       ", saved_ssid: %s, ap_ssid: %s, last_error: %s\n",
                       static_cast<uint32_t>(ip.octet1), static_cast<uint32_t>(ip.octet2),
                       static_cast<uint32_t>(ip.octet3), static_cast<uint32_t>(ip.octet4),
                       cfg.ssid[0] ? cfg.ssid : "(none)", WifiService::get_ap_ssid(),
                       WifiService::get_last_error());
                printf("espnow_active: %d, channel: %s%" PRIu32 ", tx: %" PRIu32 ", no_ack: %" PRIu32
                       ", invalid: %" PRIu32 ", timing: %" PRIu32 "\n",
                       EspNowLink::is_active() ? 1 : 0, channel_available ? "" : "N/A",
                       channel_available ? static_cast<uint32_t>(channel) : 0U,
                       static_cast<uint32_t>(now_stats.tx_packets),
                       static_cast<uint32_t>(now_stats.ack_timeouts),
                       static_cast<uint32_t>(now_stats.rx_invalid_packets),
                       static_cast<uint32_t>(now_stats.timing_errors));
                printf("sta_mac: %02X:%02X:%02X:%02X:%02X:%02X, ap_mac: %02X:%02X:%02X:%02X:%02X:%02X\n",
                       sta_mac.octet1, sta_mac.octet2, sta_mac.octet3, sta_mac.octet4, sta_mac.octet5, sta_mac.octet6,
                       ap_mac.octet1, ap_mac.octet2, ap_mac.octet3, ap_mac.octet4, ap_mac.octet5, ap_mac.octet6);
            };

            if (argc < 2 || strcmp(argv[1], "status") == 0 || strcmp(argv[1], "ip") == 0) {
                print_status();
                return 0;
            }

            if (strcmp(argv[1], "on") == 0) {
                esp_err_t ret = WifiService::start_default(TAG);
                if (ret == ESP_OK && WifiService::get_mode() != WifiService::Mode::ESPNOW_ONLY) {
                    esp_err_t web_init_ret  = WebBackend::init();
                    esp_err_t web_start_ret = WebBackend::start();
                    if (web_init_ret != ESP_OK || web_start_ret != ESP_OK) {
                        printf("web start failed: %s %s\n", esp_err_to_name(web_init_ret),
                               esp_err_to_name(web_start_ret));
                        return 1;
                    }
                }
                printf("wifi on: %s\n", esp_err_to_name(ret));
                print_status();
                return ret == ESP_OK ? 0 : 1;
            }

            if (strcmp(argv[1], "off") == 0) {
                WebBackend::stop();
                esp_err_t ret = WifiService::stop(TAG);
                printf("wifi network off, ESP-NOW remains active: %s\n", esp_err_to_name(ret));
                print_status();
                return ret == ESP_OK ? 0 : 1;
            }

            if (strcmp(argv[1], "connect") == 0) {
                if (argc < 3) {
                    printf("Usage: wifi connect <ssid> [password]\n");
                    return 1;
                }
                const char* password      = argc >= 4 ? argv[3] : "";
                esp_err_t   ret           = WifiService::connect_sta(argv[2], password, true, TAG);
                esp_err_t   web_init_ret  = ret == ESP_OK ? WebBackend::init() : ret;
                esp_err_t   web_start_ret = web_init_ret == ESP_OK ? WebBackend::start() : web_init_ret;
                if (web_init_ret != ESP_OK || web_start_ret != ESP_OK) {
                    printf("web start failed: %s %s\n", esp_err_to_name(web_init_ret), esp_err_to_name(web_start_ret));
                    return 1;
                }
                printf("wifi connect: %s\n", esp_err_to_name(ret));
                print_status();
                return ret == ESP_OK ? 0 : 1;
            }

            if (strcmp(argv[1], "ap") == 0) {
                esp_err_t ret           = WifiService::start_provision_ap(TAG);
                esp_err_t web_init_ret  = ret == ESP_OK ? WebBackend::init() : ret;
                esp_err_t web_start_ret = web_init_ret == ESP_OK ? WebBackend::start() : web_init_ret;
                if (web_init_ret != ESP_OK || web_start_ret != ESP_OK) {
                    printf("web start failed: %s %s\n", esp_err_to_name(web_init_ret), esp_err_to_name(web_start_ret));
                    return 1;
                }
                printf("wifi ap: %s\n", esp_err_to_name(ret));
                print_status();
                return ret == ESP_OK ? 0 : 1;
            }

            if (strcmp(argv[1], "boot") == 0) {
                if (argc < 3) {
                    printf("boot_enabled: %d\n", WifiService::is_web_enabled_on_boot());
                    return 0;
                }
                int enabled = atoi(argv[2]);
                if (enabled < 0 || enabled > 1) {
                    printf("Usage: wifi boot <0|1>\n");
                    return 1;
                }
                WifiService::set_web_enabled_on_boot(enabled == 1, TAG);
                printf("boot_enabled set to %d\n", enabled);
                return 0;
            }

            if (strcmp(argv[1], "clear") == 0) {
                WifiService::clear_saved_sta(TAG);
                printf("saved wifi cleared\n");
                return 0;
            }

            printf("Usage: wifi status|ip|on|off|connect <ssid> [password]|ap|boot <0|1>|clear\n");
            return 1;
        }));

    /**
     * @brief  ina228_register - 获取ina228寄存器值
     * @usage  ina228_register <register_addr>
     * @note   显示当前ina228电压电流寄存器值
     */
    shell.register_command(
        ShellCommand_t("ina228_register", "Get ina228 register value", "", [](int argc, char** argv) -> int {
            const auto state = get_global_state();
            printf("ina228_register_raw current: %d, voltage: %" PRIu32 ", available: %" PRIu32 "\n",
                   state.current_register_raw, static_cast<uint32_t>(state.voltage_register_raw),
                   static_cast<uint32_t>(state.flags.lp_ina228_initialized ? 1U : 0U));
            return 0;
        }));

    /**
     * @brief  calibration_params - 获取校准参数
     * @usage  calibration_params
     * @note   显示当前校准参数
     */
    shell.register_command(
        ShellCommand_t("calibration_params", "Get calibration params", "", [](int argc, char** argv) -> int {
            auto params = CurrentCalib::params_data.read();
            printf("Current calibration params:\n");
            printf(
                "Calibration current basek: %d == sample resistance_mOhm: %.3f\n", params.current_base_K,
                float(2500.f / params.current_base_K)); // 电流参数K与采样电阻值的计算推导见current_calibration的README
            printf("Calibration current current points:\n");
            for (int i = 0; i < sizeof(params.points) / sizeof(params.points[0]); i++) {
                printf("Calibration index %d: reg_raw_value %d,no_offset_mA %d, cali_offset_uA %d\n", i,
                       params.points[i].register_value, params.points[i].register_value * params.current_base_K / 1000,
                       params.points[i].offset_current_100uA * 100);
            }
            printf("Calibration current temperatureK: %d\n", params.temperature_K);
            return 0;
        }));

    /*============工厂模式命令==================*/
    // 校准基准电流K值
    static ShellCommand_t calibration_basek(
        "calibration_basek", "Calibration current basek Value", "<basek>", [](int argc, char** argv) -> int {
            if (argc < 2) {
                printf("Error: basek must be specified\n");
                return 1;
            }
            auto params           = CurrentCalib::params_data.read();
            int  basek            = atoi(argv[1]);
            params.current_base_K = basek;
            const esp_err_t err   = CurrentCalib::params_data.set(params);
            if (err != ESP_OK) {
                printf("Error: failed to persist calibration: %s\n", esp_err_to_name(err));
                return 1;
            }
            DEVICE_EVENT_I(TAG, "calib: base_k=%d source=shell reboot_required=1", basek);
            printf("Calibration basek set to %d restart required\n", basek);
            return 0;
        });

    // 校准温度K值
    static ShellCommand_t calibration_current_temperatureK(
        "calibration_current_temperatureK", "Calibration current current temperatureK Value", "<temperatureK>",
        [](int argc, char** argv) -> int {
            if (argc < 2) {
                printf("Error: temperatureK must be specified\n");
                return 1;
            }
            auto params          = CurrentCalib::params_data.read();
            int  temperatureK    = atoi(argv[1]);
            params.temperature_K = temperatureK;
            const esp_err_t err  = CurrentCalib::params_data.set(params);
            if (err != ESP_OK) {
                printf("Error: failed to persist calibration: %s\n", esp_err_to_name(err));
                return 1;
            }
            DEVICE_EVENT_I(TAG, "calib: temperature_k=%d source=shell reboot_required=1", temperatureK);
            printf("Calibration temperatureK set to %d restart required\n", temperatureK);
            return 0;
        });
    // 校准电流校点
    static ShellCommand_t calibration_current_points(
        "calibration_current_points", "Calibration current current points Value",
        "<point_index> <register_value> <offset_current_uA>", [](int argc, char** argv) -> int {
            if (argc < 4) {
                printf("Error: point_index, register_value, voltage_uv must be specified\n");
                return 1;
            }
            auto params      = CurrentCalib::params_data.read();
            int  point_index = atoi(argv[1]);
            if (point_index < 0 || point_index >= sizeof(params.points) / sizeof(params.points[0])) {
                printf("Error: point_index out of range\n");
                return 1;
            }

            int register_value                              = std::abs(atoi(argv[2]));
            int new_offset_current_100uA                    = atoi(argv[3]) / 100;
            params.points[point_index].register_value       = register_value;
            params.points[point_index].offset_current_100uA = new_offset_current_100uA;
            const esp_err_t err                             = CurrentCalib::params_data.set(params);
            if (err != ESP_OK) {
                printf("Error: failed to persist calibration: %s\n", esp_err_to_name(err));
                return 1;
            }
            DEVICE_EVENT_I(TAG, "calib: point=%d reg=%d offset_100ua=%d source=shell reboot_required=1", point_index,
                           register_value, new_offset_current_100uA);
            printf("Calibration current points set to %d, %d(uA) restart required\n", register_value, atoi(argv[3]));
            return 0;
        });

    static ShellCommand_t calibration_clear(
        "calibration_clear", "Clear calibration params (keep base_K)", "", [](int argc, char** argv) -> int {
            auto     params = CurrentCalib::params_data.read();
            uint16_t base_k = params.current_base_K;
            memset(params.points, 0, sizeof(params.points));
            params.temperature_K  = 0;
            params.current_base_K = base_k;
            const esp_err_t err   = CurrentCalib::params_data.set(params);
            if (err != ESP_OK) {
                printf("Error: failed to persist calibration: %s\n", esp_err_to_name(err));
                return 1;
            }
            DEVICE_EVENT_I(TAG, "calib: cleared base_k=%" PRIu32 " source=shell reboot_required=1",
                           static_cast<uint32_t>(base_k));
            printf("Calibration params cleared (base_K=%d preserved)\n", base_k);
            return 0;
        });

    shell.register_command(ShellCommand_t("factory_mode", "Enter factory mode", "", [](int argc, char** argv) -> int {
        auto& _shell = Shell::instance();
        protect_set_bypassed(true, TAG);
        _shell.register_command(calibration_basek);
        _shell.register_command(calibration_current_temperatureK);
        _shell.register_command(calibration_current_points);
        _shell.register_command(calibration_clear);
        printf("Factory mode enabled, protect bypass enabled\n");
        return 0;
    }));

    DEVICE_EVENT_I(TAG, "shell: commands state=registered result=ok");
    return ESP_OK;
}

} // namespace ShellCommand

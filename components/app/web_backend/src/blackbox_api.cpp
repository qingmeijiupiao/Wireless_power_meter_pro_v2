/*
 * @Description: 黑匣子持久化日志 Web API
 */
#include "web_backend_internal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "blackbox.h"
#include "blackbox_service.h"
#include "circular_flash_buffer.h"

namespace WebBackend {
namespace {

constexpr uint32_t DEFAULT_PAGE_LIMIT      = 8;
constexpr uint32_t MAX_PAGE_LIMIT          = 16;
constexpr uint32_t MAX_SNAPSHOT_INTERVAL_S = 86400;

bool append_checked(size_t* pos, const char* fmt, ...) {
    if (pos == nullptr || *pos >= sizeof(web_scratch_buffer)) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(web_scratch_buffer + *pos, sizeof(web_scratch_buffer) - *pos, fmt, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(web_scratch_buffer) - *pos) {
        web_scratch_buffer[sizeof(web_scratch_buffer) - 1] = '\0';
        return false;
    }
    *pos += static_cast<size_t>(n);
    return true;
}

bool append_json_escaped(size_t* pos, const char* text) {
    if (text == nullptr) {
        return true;
    }
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != '\0'; ++p) {
        switch (*p) {
        case '\"':
        case '\\':
            if (!append_checked(pos, "\\%c", *p))
                return false;
            break;
        case '\b':
            if (!append_checked(pos, "\\b"))
                return false;
            break;
        case '\f':
            if (!append_checked(pos, "\\f"))
                return false;
            break;
        case '\n':
            if (!append_checked(pos, "\\n"))
                return false;
            break;
        case '\r':
            if (!append_checked(pos, "\\r"))
                return false;
            break;
        case '\t':
            if (!append_checked(pos, "\\t"))
                return false;
            break;
        default:
            if (*p < 0x20) {
                if (!append_checked(pos, "\\u%04x", *p))
                    return false;
            } else if (!append_checked(pos, "%c", *p)) {
                return false;
            }
        }
    }
    return true;
}

void payload_to_hex(const uint8_t* payload, size_t len, char* out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < out_size; ++i) {
        int n = snprintf(out + pos, out_size - pos, "%02X", payload[i]);
        if (n != 2) {
            break;
        }
        pos += 2;
    }
    out[pos] = '\0';
}

uint32_t read_query_uint32(WebServer::Request* request, const char* key, uint32_t fallback) {
    char value[16] = {};
    if (WebServer::get_query_value(request, key, value, sizeof(value)) != ESP_OK) {
        return fallback;
    }
    char*         end    = nullptr;
    uint32_t parsed = strtoul(value, &end, 10);
    if (value[0] == '\0' || *end != '\0' || parsed > UINT32_MAX) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

bool append_snapshot(size_t* pos, const Blackbox::Record& record) {
    if (record.payload.bytes[0] != BlackboxService::SNAPSHOT_VERSION) {
        return true;
    }

    BlackboxService::SnapshotV1 snapshot = {};
    memcpy(&snapshot, record.payload.bytes, sizeof(snapshot));
    return append_checked(pos,
                          ",\"snapshot\":{\"version\":%u,\"flags\":%lu,\"protect\":%u,"
                          "\"voltage_mv\":%u,\"current_ua\":%ld,\"meter_mwh\":%.3f,"
                          "\"board_temp_c\":%.2f,\"chip_temp_c\":%.2f}",
                          static_cast<uint8_t>(snapshot.version), std::bit_cast<uint32_t>(snapshot.flags),
                          static_cast<uint8_t>(snapshot.protect_states.protect_states_raw),
                          static_cast<unsigned>(snapshot.voltage_mV), static_cast<int32_t>(snapshot.current_uA),
                          snapshot.meter_mwh, snapshot.board_temperature / 100.0, snapshot.chip_temperature / 100.0);
}

} // namespace

/** @brief GET /api/blackbox，按原始记录游标分页读取持久化日志。 */
esp_err_t blackbox_api_handler(WebServer::Request* request) {
    const uint32_t start         = read_query_uint32(request, "start", 0);
    const bool     metadata_only = read_query_uint32(request, "metadata", 0) != 0;
    uint32_t       limit         = read_query_uint32(request, "limit", DEFAULT_PAGE_LIMIT);
    if (limit == 0 || limit > MAX_PAGE_LIMIT) {
        limit = DEFAULT_PAGE_LIMIT;
    }

    if (start == 0) {
        esp_err_t ret = Blackbox::sync();
        if (ret != ESP_OK) {
            snprintf(web_scratch_buffer, sizeof(web_scratch_buffer),
                     "{\"ok\":false,\"reason\":\"%s\",\"records\":[]}\n", esp_err_to_name(ret));
            return WebServer::send_json(request, web_scratch_buffer);
        }
    }

    const uint32_t raw_count = Blackbox::count();
    uint32_t       index     = start < raw_count ? start : raw_count;
    uint32_t       emitted   = 0;
    size_t         pos       = 0;
    bool           ok        = append_checked(&pos,
                                              "{\"ok\":true,\"enabled\":%s,\"persisted_records\":%lu,"
                                                               "\"capacity_records\":%lu,\"snapshot_interval_s\":%lu,\"start\":%lu,\"records\":[",
                             Blackbox::is_enabled() ? "true" : "false", static_cast<uint32_t>(raw_count),
                                              static_cast<uint32_t>(Blackbox::capacity()),
                                              static_cast<uint32_t>(BlackboxService::get_snapshot_interval_s()),
                                              static_cast<uint32_t>(index));

    while (ok && !metadata_only && index < raw_count && emitted < limit) {
        const Blackbox::Record record    = Blackbox::read(index);
        const char*            separator = emitted == 0 ? "" : ",";
        if (record.header.sof != CircularFlashBuffer::BLOCK_SOF) {
            ok = append_checked(&pos, "%s{\"index\":%lu,\"type\":\"invalid\"}", separator,
                                static_cast<uint32_t>(index));
            ++index;
            ++emitted;
            continue;
        }

        if (record.header.type == Blackbox::LogType::STRING) {
            const Blackbox::TextRecord text = Blackbox::read_text(index);
            if (text.record_count != 0) {
                ok = append_checked(&pos,
                                    "%s{\"index\":%lu,\"timestamp_ms\":%lu,\"type\":\"string\","
                                    "\"fragments\":%u,\"text\":\"",
                                    separator, static_cast<uint32_t>(index),
                                    static_cast<uint32_t>(record.header.timestamp),
                                    static_cast<uint32_t>(text.record_count));
                if (ok)
                    ok = append_json_escaped(&pos, text.str);
                if (ok)
                    ok = append_checked(&pos, "\"}");
                index += text.record_count;
                ++emitted;
                continue;
            }
        }

        char payload_hex[Blackbox::PAYLOAD_SIZE * 2 + 1] = {};
        payload_to_hex(record.payload.bytes, sizeof(record.payload.bytes), payload_hex, sizeof(payload_hex));
        ok = append_checked(
            &pos, "%s{\"index\":%lu,\"timestamp_ms\":%lu,\"type\":\"%s\",\"payload_hex\":\"%s\"", separator,
            static_cast<uint32_t>(index), static_cast<uint32_t>(record.header.timestamp),
            record.header.type == Blackbox::LogType::STRUCTURED ? "structured" : "unknown", payload_hex);
        if (ok && record.header.type == Blackbox::LogType::STRUCTURED) {
            ok = append_snapshot(&pos, record);
        }
        if (ok)
            ok = append_checked(&pos, "}");
        ++index;
        ++emitted;
    }

    if (ok) {
        ok = append_checked(&pos, "],\"next\":%lu,\"has_more\":%s}\n", static_cast<uint32_t>(index),
                            !metadata_only && index < raw_count ? "true" : "false");
    }
    if (!ok) {
        snprintf(web_scratch_buffer, sizeof(web_scratch_buffer),
                 "{\"ok\":false,\"reason\":\"response_too_large\",\"records\":[]}\n");
    }
    return WebServer::send_json(request, web_scratch_buffer);
}

/** @brief POST /api/blackbox/clear，清空持久化日志并保留 reset 标记。 */
esp_err_t blackbox_clear_handler(WebServer::Request* request) {
    const esp_err_t ret = Blackbox::erase_all();
    snprintf(web_scratch_buffer, sizeof(web_scratch_buffer),
             "{\"ok\":%s,\"reason\":\"%s\",\"persisted_records\":%lu}\n", ret == ESP_OK ? "true" : "false",
             ret == ESP_OK ? "ok" : esp_err_to_name(ret), static_cast<uint32_t>(Blackbox::count()));
    return WebServer::send_json(request, web_scratch_buffer);
}

/** @brief POST /api/blackbox/config，更新周期快照间隔。 */
esp_err_t blackbox_config_handler(WebServer::Request* request) {
    esp_err_t ret = WebServer::load_body(request);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t seconds = 0;
    if (!json_get_uint32(request->body, "snapshot_interval_s", &seconds) || seconds > MAX_SNAPSHOT_INTERVAL_S) {
        return WebServer::send(request, 400, "application/json",
                               "{\"ok\":false,\"reason\":\"invalid_snapshot_interval\"}\n",
                               strlen("{\"ok\":false,\"reason\":\"invalid_snapshot_interval\"}\n"));
    }

    const esp_err_t config_err = BlackboxService::set_snapshot_interval_s(seconds, "WebBackend");
    if (config_err != ESP_OK) {
        return WebServer::send(request, 500, "application/json", "{\"ok\":false,\"reason\":\"persist_failed\"}\n",
                               strlen("{\"ok\":false,\"reason\":\"persist_failed\"}\n"));
    }
    snprintf(web_scratch_buffer, sizeof(web_scratch_buffer), "{\"ok\":true,\"snapshot_interval_s\":%lu}\n",
             static_cast<uint32_t>(BlackboxService::get_snapshot_interval_s()));
    return WebServer::send_json(request, web_scratch_buffer);
}

} // namespace WebBackend

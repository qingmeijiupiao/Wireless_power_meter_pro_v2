#include "ota_service.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#include "diagnostic_log.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#define JSMN_STATIC
#include "jsmn.h"
#include "ota_manager.h"

namespace OtaService {
namespace {

constexpr char     TAG[]                 = "OtaService";
constexpr char     MANIFEST_PATH[]       = "/gh/qingmeijiupiao/"
                                           "Wireless_power_meter_pro_v2@firmware-dist/ota/latest.json";
constexpr size_t   CONFIG_BUFFER_SIZE    = 4096;
constexpr size_t   DOWNLOAD_BUFFER_SIZE  = 4096;
constexpr uint32_t HTTP_TIMEOUT_MS       = 5000;
constexpr uint32_t TASK_STACK_SIZE       = 10240;
constexpr uint8_t  MAX_DOWNLOAD_ATTEMPTS = 4;
constexpr uint32_t RETRY_DELAY_MS        = 1000;

/**
 * @brief jsDelivr 镜像源列表。
 *
 * cdn.jsdelivr.net 解析出的边缘 IP 可能不可达，而 ESP-IDF v6.0 的
 * esp_tls_hostname_to_fd() 只使用 getaddrinfo 返回的第一条记录，没有 IP 级
 * 回退。不同主机名会触发各自独立的 DNS 解析，因此按顺序轮换主机名可以绕开
 * 单个不可达的边缘节点。
 */
struct ManifestSource {
    const char* name;
    const char* base;
};

constexpr ManifestSource SOURCES[] = {
    {"fastly", "https://fastly.jsdelivr.net"},
    {"gcore", "https://gcore.jsdelivr.net"},
    {"jsdelivr", "https://cdn.jsdelivr.net"},
};
constexpr size_t SOURCE_COUNT = sizeof(SOURCES) / sizeof(SOURCES[0]);

enum class Operation : uint8_t {
    CHECK,
    UPGRADE,
};

struct Version {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
};

SemaphoreHandle_t status_mutex = nullptr;
TaskHandle_t      worker_task  = nullptr;
Status            status       = {};
char              config_buffer[CONFIG_BUFFER_SIZE];
char              firmware_url[384];
size_t            firmware_size         = 0;
size_t            manifest_source_index = 0;

/**
 * @brief 以 ASCII 大小写不敏感方式比较两个字符串。
 *
 * HTTP Header
 * 字段名不区分大小写，不能依赖服务端固定返回 Content-Range 的拼写。
 */
bool ascii_equals_ignore_case(const char* lhs, const char* rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        const char lhs_lower = *lhs >= 'A' && *lhs <= 'Z' ? static_cast<char>(*lhs + ('a' - 'A')) : *lhs;
        const char rhs_lower = *rhs >= 'A' && *rhs <= 'Z' ? static_cast<char>(*rhs + ('a' - 'A')) : *rhs;
        if (lhs_lower != rhs_lower) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == *rhs;
}

void lock_status() {
    xSemaphoreTake(status_mutex, portMAX_DELAY);
}

void unlock_status() {
    xSemaphoreGive(status_mutex);
}

void set_state(State state, const char* error = "") {
    lock_status();
    const State old_state = status.state;
    status.state          = state;
    snprintf(status.last_error, sizeof(status.last_error), "%s", error == nullptr ? "" : error);
    unlock_status();
    if (old_state == state && (error == nullptr || error[0] == '\0')) {
        return;
    }
    if (state == State::FAILED) {
        DEVICE_STATE_W(TAG, "ota: state old=%s new=%s reason=%s", state_to_string(old_state), state_to_string(state),
                       error == nullptr || error[0] == '\0' ? "unknown" : error);
    } else {
        DEVICE_STATE_I(TAG, "ota: state old=%s new=%s result=ok", state_to_string(old_state), state_to_string(state));
    }
}

void set_active_source(const char* source) {
    lock_status();
    snprintf(status.active_source, sizeof(status.active_source), "%s", source == nullptr ? "" : source);
    unlock_status();
}

void set_progress(size_t downloaded, size_t image_size) {
    lock_status();
    status.bytes_downloaded = downloaded;
    status.image_size       = image_size;
    unlock_status();
}

bool parse_version(const char* text, Version* version) {
    if (text == nullptr || version == nullptr) {
        return false;
    }
    uint32_t    major = 0;
    uint32_t    minor = 0;
    uint32_t    patch = 0;
    const char* start = text[0] == 'v' ? text + 1 : text;
    char        tail  = '\0';
    if (sscanf(start, "%" SCNu32 ".%" SCNu32 ".%" SCNu32 "%c", &major, &minor, &patch, &tail) != 3) {
        return false;
    }
    version->major = major;
    version->minor = minor;
    version->patch = patch;
    return true;
}

int compare_version(const Version& lhs, const Version& rhs) {
    if (lhs.major != rhs.major) {
        return lhs.major < rhs.major ? -1 : 1;
    }
    if (lhs.minor != rhs.minor) {
        return lhs.minor < rhs.minor ? -1 : 1;
    }
    if (lhs.patch != rhs.patch) {
        return lhs.patch < rhs.patch ? -1 : 1;
    }
    return 0;
}

esp_http_client_handle_t open_http(const char* url, int64_t* content_length) {
    esp_http_client_config_t config = {};
    config.url                      = url;
    config.timeout_ms               = HTTP_TIMEOUT_MS;
    config.buffer_size              = DOWNLOAD_BUFFER_SIZE;
    config.crt_bundle_attach        = esp_crt_bundle_attach;
    config.keep_alive_enable        = true;
    config.max_redirection_count    = 5;
    config.tls_dyn_buf_strategy     = HTTP_TLS_DYN_BUF_RX_STATIC;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return nullptr;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return nullptr;
    }

    const int64_t length      = esp_http_client_fetch_headers(client);
    const int     status_code = esp_http_client_get_status_code(client);
    if (length < 0 || status_code < 200 || status_code >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return nullptr;
    }
    if (content_length != nullptr) {
        *content_length = length;
    }
    return client;
}

void close_http(esp_http_client_handle_t client) {
    if (client == nullptr) {
        return;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

esp_err_t fetch_manifest(char* output, size_t output_size) {
    for (size_t index = 0; index < SOURCE_COUNT; ++index) {
        char url[384] = {};
        snprintf(url, sizeof(url), "%s%s", SOURCES[index].base, MANIFEST_PATH);
        set_active_source(SOURCES[index].name);
        DEVICE_EVENT_I(TAG, "ota: manifest_attempt source=%s free=%u largest=%u url=%s", SOURCES[index].name,
                       static_cast<unsigned>(esp_get_free_heap_size()),
                       static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)), url);

        int64_t                  content_length = 0;
        esp_http_client_handle_t client         = open_http(url, &content_length);
        if (client == nullptr) {
            ESP_LOGW(TAG, "ota: manifest source=%s result=failed reason=http_open", SOURCES[index].name);
            continue;
        }
        if (content_length >= static_cast<int64_t>(output_size)) {
            close_http(client);
            ESP_LOGW(TAG, "ota: manifest source=%s result=failed reason=too_large", SOURCES[index].name);
            continue;
        }

        size_t total       = 0;
        bool   read_failed = false;
        while (total + 1 < output_size) {
            const int read = esp_http_client_read(client, output + total, static_cast<int>(output_size - total - 1));
            if (read < 0) {
                read_failed = true;
                break;
            }
            if (read == 0) {
                break;
            }
            total += static_cast<size_t>(read);
        }
        close_http(client);
        output[total] = '\0';
        if (read_failed || total == 0) {
            ESP_LOGW(TAG, "ota: manifest source=%s result=failed reason=read", SOURCES[index].name);
            continue;
        }
        DEVICE_EVENT_I(TAG, "ota: manifest source=%s result=ok bytes=%" PRIu32, SOURCES[index].name,
                       static_cast<uint32_t>(total));
        manifest_source_index = index;
        return ESP_OK;
    }
    return ESP_FAIL;
}

bool token_equals(const char* json, const jsmntok_t& token, const char* text) {
    const size_t token_size = static_cast<size_t>(token.end - token.start);
    return token.type == JSMN_STRING && strlen(text) == token_size &&
           strncmp(json + token.start, text, token_size) == 0;
}

const jsmntok_t* find_value_token(const char* json, const jsmntok_t* tokens, int token_count, const char* key) {
    for (int index = 1; index + 1 < token_count; ++index) {
        if (token_equals(json, tokens[index], key)) {
            return &tokens[index + 1];
        }
    }
    return nullptr;
}

bool copy_string_token(const char* json, const jsmntok_t* token, char* output, size_t output_size) {
    if (token == nullptr || token->type != JSMN_STRING || token->start < 0 || token->end < token->start) {
        return false;
    }
    const size_t length = static_cast<size_t>(token->end - token->start);
    if (length >= output_size) {
        return false;
    }
    memcpy(output, json + token->start, length);
    output[length] = '\0';
    return true;
}

bool parse_size_token(const char* json, const jsmntok_t* token, size_t* output) {
    if (token == nullptr || output == nullptr || token->type != JSMN_PRIMITIVE || token->start < 0 ||
        token->end < token->start) {
        return false;
    }
    const size_t length = static_cast<size_t>(token->end - token->start);
    if (length == 0 || length >= 32) {
        return false;
    }
    char text[32] = {};
    memcpy(text, json + token->start, length);

    uint64_t parsed = 0;
    char               tail   = '\0';
    if (sscanf(text, "%" SCNu64 "%c", &parsed, &tail) != 1 || parsed == 0 || parsed > SIZE_MAX) {
        return false;
    }
    *output = static_cast<size_t>(parsed);
    return true;
}

bool parse_manifest(const char* manifest, char* version, size_t version_size, char* url, size_t url_size,
                    size_t* image_size) {
    if (manifest == nullptr || version == nullptr || url == nullptr || image_size == nullptr) {
        return false;
    }

    jsmn_parser parser     = {};
    jsmntok_t   tokens[32] = {};
    jsmn_init(&parser);
    const int token_count = jsmn_parse(&parser, manifest, strlen(manifest), tokens, sizeof(tokens) / sizeof(tokens[0]));
    if (token_count < 1 || tokens[0].type != JSMN_OBJECT) {
        return false;
    }

    const jsmntok_t* schema_token   = find_value_token(manifest, tokens, token_count, "schema_version");
    const jsmntok_t* version_token  = find_value_token(manifest, tokens, token_count, "version");
    const jsmntok_t* firmware_token = find_value_token(manifest, tokens, token_count, "firmware");
    const jsmntok_t* url_token      = find_value_token(manifest, tokens, token_count, "url");
    const jsmntok_t* size_token     = find_value_token(manifest, tokens, token_count, "size");

    char schema[8] = {};
    if (firmware_token == nullptr || firmware_token->type != JSMN_OBJECT || schema_token == nullptr ||
        schema_token->type != JSMN_PRIMITIVE || !copy_string_token(manifest, version_token, version, version_size) ||
        !copy_string_token(manifest, url_token, url, url_size) || !parse_size_token(manifest, size_token, image_size)) {
        return false;
    }

    const size_t schema_length = static_cast<size_t>(schema_token->end - schema_token->start);
    if (schema_length >= sizeof(schema)) {
        return false;
    }
    memcpy(schema, manifest + schema_token->start, schema_length);
    if (strcmp(schema, "1") != 0 || strncmp(url, "https://", 8) != 0) {
        return false;
    }

    Version parsed = {};
    return parse_version(version, &parsed);
}

esp_err_t check_latest_version() {
    set_state(State::CHECKING);
    set_progress(0, 0);

    constexpr time_t MIN_VALID_TLS_TIME = 1704067200; // 2024-01-01 UTC
    if (time(nullptr) < MIN_VALID_TLS_TIME) {
        set_state(State::FAILED, "time_not_synchronized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = fetch_manifest(config_buffer, sizeof(config_buffer));
    if (err != ESP_OK) {
        set_state(State::FAILED, "manifest_download_failed");
        return err;
    }

    char   latest[32]                       = {};
    char   latest_url[sizeof(firmware_url)] = {};
    size_t latest_size                      = 0;
    if (!parse_manifest(config_buffer, latest, sizeof(latest), latest_url, sizeof(latest_url), &latest_size)) {
        ESP_LOGW(TAG, "remote manifest parse failed");
        set_state(State::FAILED, "manifest_invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "remote manifest parsed latest=%s size=%" PRIu32, latest, static_cast<uint32_t>(latest_size));

    const esp_app_desc_t* running         = esp_app_get_description();
    Version               current_version = {};
    Version               latest_version  = {};
    if (!parse_version(running->version, &current_version) || !parse_version(latest, &latest_version)) {
        set_state(State::FAILED, "version_invalid");
        return ESP_ERR_INVALID_VERSION;
    }

    lock_status();
    const State previous_state = status.state;
    snprintf(status.current_version, sizeof(status.current_version), "%s", running->version);
    snprintf(status.latest_version, sizeof(status.latest_version), "%s", latest);
    snprintf(firmware_url, sizeof(firmware_url), "%s", latest_url);
    firmware_size           = latest_size;
    status.active_source[0] = '\0';
    status.last_error[0]    = '\0';
    status.state = compare_version(latest_version, current_version) > 0 ? State::UPDATE_AVAILABLE : State::UP_TO_DATE;
    const State result_state = status.state;
    unlock_status();

    DEVICE_STATE_I(TAG, "ota: state old=%s new=%s result=ok", state_to_string(previous_state),
                   state_to_string(result_state));
    DEVICE_EVENT_I(TAG, "ota: remote_check current=%s latest=%s result=%s", running->version, latest,
                   state_to_string(result_state));
    return ESP_OK;
}

struct DownloadContext {
    esp_err_t error;
    size_t    total;
    size_t    image_size;
    size_t    expected_size;
    size_t    request_offset; /**< 本次 HTTP 请求从固件中的哪个偏移开始。 */
    bool      ota_started;
    bool      range_header_seen; /**< 续传响应是否包含 Content-Range。 */
    bool      range_valid;       /**< Content-Range 是否与请求偏移和总长度一致。 */
};

/**
 * @brief 接收固件 HTTP 事件并按顺序写入 OTA 分区。
 *
 * 首次下载要求 HTTP 200；续传要求 HTTP
 * 206，并严格校验
 * Content-Range，
 * 防止服务端忽略 Range 后从头返回固件而导致目标分区内容重复。
 */
esp_err_t firmware_http_event(esp_http_client_event_t* event) {
    auto* context = static_cast<DownloadContext*>(event->user_data);
    if (context == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (event->event_id == HTTP_EVENT_ON_HEADER && context->request_offset > 0 && event->header_key != nullptr &&
        event->header_value != nullptr && ascii_equals_ignore_case(event->header_key, "Content-Range")) {
        uint64_t start   = 0;
        uint64_t end     = 0;
        uint64_t total   = 0;
        char               tail    = '\0';
        context->range_header_seen = true;
        context->range_valid =
            sscanf(event->header_value, "bytes %" SCNu64 "-%" SCNu64 "/%" SCNu64 "%c", &start, &end, &total,
                   &tail) == 3 &&
            start == context->request_offset && total == context->expected_size && end >= start && end < total;
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    const int status_code = esp_http_client_get_status_code(event->client);
    if ((context->request_offset == 0 && status_code != 200) ||
        (context->request_offset > 0 && (status_code != 206 || !context->range_header_seen || !context->range_valid))) {
        context->error = ESP_ERR_INVALID_RESPONSE;
        return context->error;
    }

    const size_t data_size = static_cast<size_t>(event->data_len);
    if (data_size > context->expected_size - context->total) {
        context->error = ESP_ERR_INVALID_SIZE;
        return context->error;
    }

    if (context->total == context->request_offset) {
        const int64_t content_length = esp_http_client_get_content_length(event->client);
        if (content_length > 0 &&
            static_cast<uint64_t>(content_length) != context->expected_size - context->request_offset) {
            context->error = ESP_ERR_INVALID_SIZE;
            return context->error;
        }
    }

    if (!context->ota_started) {
        context->image_size = context->expected_size;
        context->error      = OtaManager::begin(context->expected_size);
        if (context->error != ESP_OK) {
            return context->error;
        }
        context->ota_started = true;
        set_progress(0, context->image_size);
    }

    context->error = OtaManager::write(event->data, data_size);
    if (context->error != ESP_OK) {
        return context->error;
    }
    context->total += data_size;
    set_progress(context->total, context->image_size);
    return ESP_OK;
}

/** @brief 提取完整 URL 的 path（含前导 '/'），用于拼接各镜像源。 */
const char* url_path(const char* url) {
    if (url == nullptr) {
        return nullptr;
    }
    const char* scheme = strstr(url, "://");
    if (scheme == nullptr) {
        return nullptr;
    }
    return strchr(scheme + 3, '/');
}

esp_err_t download_firmware(const char* url, const char* version, size_t expected_size) {
    const char* path = url_path(url);
    if (path == nullptr) {
        set_state(State::FAILED, "firmware_url_invalid");
        return ESP_ERR_INVALID_ARG;
    }

    DownloadContext context = {
        .error             = ESP_OK,
        .total             = 0,
        .image_size        = 0,
        .expected_size     = expected_size,
        .request_offset    = 0,
        .ota_started       = false,
        .range_header_seen = false,
        .range_valid       = false,
    };

    set_state(State::DOWNLOADING);
    set_progress(0, 0);

    esp_err_t   err         = ESP_FAIL;
    const char* active_name = "none";
    // 先在完成 manifest 的镜像源上用 Range 断点续传（同一源内容一致）；只有该源
    // 整体失败后才换源，换源时立即从头重下，避免不同镜像返回的固件内容不一致。
    for (size_t source_step = 0; source_step < SOURCE_COUNT && context.total < expected_size; ++source_step) {
        const size_t          source_index = (manifest_source_index + source_step) % SOURCE_COUNT;
        const ManifestSource& source       = SOURCES[source_index];
        active_name                        = source.name;

        if (source_step > 0) {
            if (context.ota_started) {
                OtaManager::abort();
            }
            context.error             = ESP_OK;
            context.total             = 0;
            context.image_size        = 0;
            context.ota_started       = false;
            context.range_header_seen = false;
            context.range_valid       = false;
            set_progress(0, 0);
        }

        char attempt_url[448] = {};
        snprintf(attempt_url, sizeof(attempt_url), "%s%s", source.base, path);

        for (uint8_t attempt = 1; attempt <= MAX_DOWNLOAD_ATTEMPTS && context.total < expected_size; ++attempt) {
            context.error             = ESP_OK;
            context.request_offset    = context.total;
            context.range_header_seen = false;
            context.range_valid       = context.request_offset == 0;

            set_active_source(source.name);
            DEVICE_EVENT_I(TAG, "ota: firmware_attempt source=%s version=%s offset=%" PRIu32 " url=%s", source.name,
                           version, static_cast<uint32_t>(context.request_offset), attempt_url);

            esp_http_client_config_t config = {};
            config.url                      = attempt_url;
            config.timeout_ms               = HTTP_TIMEOUT_MS;
            config.buffer_size              = DOWNLOAD_BUFFER_SIZE;
            config.crt_bundle_attach        = esp_crt_bundle_attach;
            config.event_handler            = firmware_http_event;
            config.user_data                = &context;
            config.keep_alive_enable        = true;
            config.max_redirection_count    = 5;
            config.tls_dyn_buf_strategy     = HTTP_TLS_DYN_BUF_RX_STATIC;

            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (client == nullptr) {
                err = ESP_ERR_NO_MEM;
                break;
            }

            char range_header[48] = {};
            if (context.request_offset > 0) {
                snprintf(range_header, sizeof(range_header), "bytes=%" PRIu32 "-",
                         static_cast<uint32_t>(context.request_offset));
                err = esp_http_client_set_header(client, "Range", range_header);
                if (err != ESP_OK) {
                    esp_http_client_cleanup(client);
                    break;
                }
            }

            DEVICE_EVENT_I(TAG,
                           "ota: http_attempt source=%s attempt=%" PRIu32 "/%" PRIu32 " offset=%" PRIu32
                           " expected=%" PRIu32,
                           source.name, static_cast<uint32_t>(attempt), static_cast<uint32_t>(MAX_DOWNLOAD_ATTEMPTS),
                           static_cast<uint32_t>(context.request_offset), static_cast<uint32_t>(expected_size));
            const size_t before_attempt = context.total;
            err                         = esp_http_client_perform(client);
            const int status_code       = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            if (context.error != ESP_OK) {
                err = context.error;
            }
            if (err == ESP_OK) {
                const int expected_status = context.request_offset == 0 ? 200 : 206;
                if (status_code != expected_status || !context.ota_started) {
                    err = ESP_ERR_INVALID_RESPONSE;
                } else if (context.total != expected_size) {
                    err = ESP_ERR_HTTP_INCOMPLETE_DATA;
                }
            }
            if (err == ESP_OK) {
                break;
            }

            ESP_LOGW(TAG,
                     "ota: http_attempt source=%s attempt=%" PRIu32 " result=failed status=%d "
                     "bytes=%" PRIu32 "/%" PRIu32 " gained=%" PRIu32 " err=%s",
                     source.name, static_cast<uint32_t>(attempt), status_code, static_cast<uint32_t>(context.total),
                     static_cast<uint32_t>(expected_size), static_cast<uint32_t>(context.total - before_attempt),
                     esp_err_to_name(err));

            // 断流（读超时/长度不完整）可在同一镜像源上续传重试；协议或写入错误则换源。
            const bool protocol_or_write_error =
                err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_SIZE ||
                context.error == ESP_ERR_INVALID_RESPONSE || context.error == ESP_ERR_INVALID_SIZE ||
                (context.error != ESP_OK && context.error != ESP_ERR_HTTP_INCOMPLETE_DATA &&
                 context.error != ESP_ERR_HTTP_READ_TIMEOUT);
            if (protocol_or_write_error) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS * attempt));
        }

        if (err == ESP_OK && context.total == expected_size) {
            break;
        }
        err = ESP_FAIL;
    }

    if (err == ESP_OK && context.total != expected_size) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        if (context.ota_started) {
            OtaManager::abort();
        }
        ESP_LOGW(TAG, "ota: firmware source=%s result=failed bytes=%" PRIu32 "/%" PRIu32 " err=%s", active_name,
                 static_cast<uint32_t>(context.total), static_cast<uint32_t>(expected_size), esp_err_to_name(err));
        return err;
    }

    set_state(State::VERIFYING);
    err = OtaManager::finish();
    if (err == ESP_OK) {
        err = OtaManager::activate();
    }
    if (err != ESP_OK) {
        if (OtaManager::get_status().state == OtaManager::State::VERIFIED) {
            OtaManager::abort();
        }
        ESP_LOGE(TAG, "ota: firmware_verify source=%s result=failed err=%s", active_name, esp_err_to_name(err));
        return err;
    }

    DEVICE_STATE_I(TAG, "ota: firmware source=%s result=activated version=%s bytes=%" PRIu32, active_name, version,
                   static_cast<uint32_t>(context.total));
    return ESP_OK;
}

esp_err_t perform_upgrade() {
    if (OtaManager::get_status().state != OtaManager::State::IDLE) {
        set_state(State::FAILED, "ota_busy");
        return ESP_ERR_INVALID_STATE;
    }

    Status snapshot = get_status();
    if (snapshot.state != State::UPDATE_AVAILABLE) {
        esp_err_t err = check_latest_version();
        if (err != ESP_OK) {
            return err;
        }
        snapshot = get_status();
    }
    if (snapshot.state != State::UPDATE_AVAILABLE) {
        return ESP_ERR_INVALID_VERSION;
    }

    char   download_url[sizeof(firmware_url)] = {};
    size_t expected_size                      = 0;
    lock_status();
    snprintf(download_url, sizeof(download_url), "%s", firmware_url);
    expected_size = firmware_size;
    unlock_status();

    const esp_err_t err = download_firmware(download_url, snapshot.latest_version, expected_size);
    if (err == ESP_OK) {
        set_state(State::RESTARTING);
        DEVICE_EVENT_I(TAG, "ota: restart delay_ms=2000 reason=upgrade_complete");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    set_state(State::FAILED, esp_err_to_name(err));
    set_active_source("");
    ESP_LOGE(TAG, "firmware download failed: %s", esp_err_to_name(err));
    return err;
}

void worker(void* argument) {
    const Operation operation = *static_cast<Operation*>(argument);
    delete static_cast<Operation*>(argument);

    if (operation == Operation::CHECK) {
        check_latest_version();
    } else {
        perform_upgrade();
    }

    lock_status();
    worker_task = nullptr;
    unlock_status();
    vTaskDelete(nullptr);
}

esp_err_t start_operation(Operation operation) {
    ESP_RETURN_ON_ERROR(init(), TAG, "init failed");

    lock_status();
    if (worker_task != nullptr) {
        unlock_status();
        return ESP_ERR_INVALID_STATE;
    }
    Operation* argument = new (std::nothrow) Operation(operation);
    if (argument == nullptr) {
        unlock_status();
        return ESP_ERR_NO_MEM;
    }
    const BaseType_t result = xTaskCreate(worker, "ota_remote", TASK_STACK_SIZE, argument, 3, &worker_task);
    if (result != pdPASS) {
        worker_task = nullptr;
        delete argument;
        unlock_status();
        return ESP_ERR_NO_MEM;
    }
    unlock_status();
    return ESP_OK;
}

} // namespace

esp_err_t init() {
    if (status_mutex != nullptr) {
        return ESP_OK;
    }
    status_mutex = xSemaphoreCreateMutex();
    if (status_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_app_desc_t* running = esp_app_get_description();
    status.state                  = State::IDLE;
    snprintf(status.current_version, sizeof(status.current_version), "%s", running->version);
    return ESP_OK;
}

esp_err_t request_check() {
    return start_operation(Operation::CHECK);
}

esp_err_t request_upgrade() {
    return start_operation(Operation::UPGRADE);
}

Status get_status() {
    Status snapshot = {};
    if (status_mutex == nullptr) {
        const esp_app_desc_t* running = esp_app_get_description();
        snapshot.state                = State::IDLE;
        snprintf(snapshot.current_version, sizeof(snapshot.current_version), "%s", running->version);
        return snapshot;
    }
    lock_status();
    snapshot = status;
    unlock_status();
    return snapshot;
}

const char* state_to_string(State state) {
    switch (state) {
    case State::IDLE:
        return "idle";
    case State::CHECKING:
        return "checking";
    case State::UPDATE_AVAILABLE:
        return "update_available";
    case State::UP_TO_DATE:
        return "up_to_date";
    case State::DOWNLOADING:
        return "downloading";
    case State::VERIFYING:
        return "verifying";
    case State::RESTARTING:
        return "restarting";
    case State::FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

} // namespace OtaService

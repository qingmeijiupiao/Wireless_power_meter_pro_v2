/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: Web 后端组件内部接口，仅供 web_backend 组件内多个源文件共享
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-05-29
 */
#ifndef WEB_BACKEND_INTERNAL_H
#define WEB_BACKEND_INTERNAL_H

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_partition.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "wifi_service.h"

namespace WebBackend {

/*
 * HTTP server 串行分发请求，各 handler 共用一块响应/流式上传缓冲。
 * 响应发送在 handler
 * 返回前完成，不允许后台任务保留该缓冲区指针。
 */
constexpr size_t WEB_SCRATCH_BUFFER_SIZE = 8192;
extern char      web_scratch_buffer[WEB_SCRATCH_BUFFER_SIZE];

// 现有 handler 按响应规模使用这些名称，底层均指向同一串行 scratch buffer。
inline char (&response_buffer)[WEB_SCRATCH_BUFFER_SIZE]        = web_scratch_buffer;
inline char (&detail_response_buffer)[WEB_SCRATCH_BUFFER_SIZE] = web_scratch_buffer;
inline char (&scan_response_buffer)[WEB_SCRATCH_BUFFER_SIZE]   = web_scratch_buffer;

/** @brief 安装 ESP_LOG 捕获钩子，将实时日志写入 RAM 环形缓冲。 */
void install_log_capture();

/** @brief 清空实时日志 RAM 环形缓冲。 */
void clear_log_ring();

/**
 * @brief 按日志序号增量读取 RAM 环形日志。
 *
 * @param since 客户端已读到的序号，服务端从该序号之后返回数据
 * @param out 输出缓冲区
 * @param out_size 输出缓冲区长度
 * @param from_seq 实际读取起始序号
 * @param next_seq 下次读取应传入的序号
 * @param latest_seq 当前最新日志序号
 * @param dropped true 表示 since 早于环形缓冲保留范围，旧日志已被覆盖
 * @return 实际复制到 out 的字节数
 */
size_t read_log_ring(uint64_t since, char* out, size_t out_size, uint64_t* from_seq, uint64_t* next_seq,
                     uint64_t* latest_seq, bool* dropped);

/** @brief 从 JSON 请求体中读取字符串字段，结果复制到调用方提供的固定缓冲。 */
bool json_get_string(const char* json, const char* key, char* out, size_t out_size);

/** @brief 从 JSON 请求体中读取布尔字段。 */
bool json_get_bool(const char* json, const char* key, bool* out);

/** @brief 从 JSON 请求体中读取 uint32 字段。 */
bool json_get_uint32(const char* json, const char* key, uint32_t* out);

/** @brief 从 JSON 请求体中读取 int32 字段，支持负号。 */
bool json_get_int32(const char* json, const char* key, int32_t* out);

/** @brief 判断 JSON 请求体中是否存在指定字段。 */
bool json_has_key(const char* json, const char* key);

/** @brief 将 WiFiService 模式转换为前端 API 使用的短字符串。 */
const char* mode_to_str(WifiService::Mode mode);

/** @brief 将 IP_t 转换为点分十进制字符串。 */
void ip_to_str(IP_t ip, char* out, size_t out_size);

/*
 * WebServer::Middleware 会在路由 handler 之前执行，适合做日志、鉴权、CORS 等横切逻辑。
 * 当前组件只做请求日志，CORS 放在 web_backend.cpp 内，便于和路由注册一起阅读。
 */
esp_err_t log_middleware(WebServer::Request* request);

/** @brief 返回入口页面或按网络模式重定向。 */
esp_err_t index_handler(WebServer::Request* request);
/** @brief 返回 Web 主页面。 */
esp_err_t main_page_handler(WebServer::Request* request);
/** @brief 返回趋势曲线页面。 */
esp_err_t charts_page_handler(WebServer::Request* request);
/** @brief 返回设备控制页面。 */
esp_err_t control_page_handler(WebServer::Request* request);
/** @brief 返回系统状态页面。 */
esp_err_t status_page_handler(WebServer::Request* request);
/** @brief 返回实时日志页面。 */
esp_err_t logs_page_handler(WebServer::Request* request);
/** @brief 返回黑匣子历史页面。 */
esp_err_t blackbox_page_handler(WebServer::Request* request);
/** @brief 返回固件升级页面。 */
esp_err_t firmware_page_handler(WebServer::Request* request);
/** @brief 返回 Web 公共样式表。 */
esp_err_t app_css_handler(WebServer::Request* request);
/** @brief 返回 WiFi 配网页面。 */
esp_err_t provision_handler(WebServer::Request* request);

/** @brief 返回设备实时状态。 */
esp_err_t state_handler(WebServer::Request* request);
/** @brief 处理输出开关请求。 */
esp_err_t output_handler(WebServer::Request* request);
/** @brief 执行一次诊断短路检测，不提交主输出状态。 */
esp_err_t short_test_handler(WebServer::Request* request);
/** @brief 重置共享计量会话。 */
esp_err_t meter_reset_handler(WebServer::Request* request);
/** @brief 安排设备重启。 */
esp_err_t reboot_handler(WebServer::Request* request);
/** @brief 返回系统与固件信息。 */
esp_err_t system_handler(WebServer::Request* request);
/** @brief 查询或设置屏幕背光。 */
esp_err_t backlight_handler(WebServer::Request* request);
/** @brief 查询或设置开机画面时长。 */
esp_err_t start_logo_handler(WebServer::Request* request);
/** @brief 查询或更新保护配置。 */
esp_err_t protect_handler(WebServer::Request* request);
/** @brief 查询或更新 CAN 配置。 */
esp_err_t can_handler(WebServer::Request* request);
/** @brief 查询或更新电流校准参数。 */
esp_err_t calibration_handler(WebServer::Request* request);
esp_err_t voltage_calibration_handler(WebServer::Request* request);
/** @brief 返回底层测量诊断信息。 */
esp_err_t diagnostics_handler(WebServer::Request* request);
/** @brief 采样并返回 RTOS 任务统计。 */
esp_err_t rtos_stats_handler(WebServer::Request* request);
/** @brief 返回 WiFi 与 ESP-NOW 状态。 */
esp_err_t wifi_status_handler(WebServer::Request* request);
/** @brief 增量返回实时日志。 */
esp_err_t logs_api_handler(WebServer::Request* request);
/** @brief 清空实时日志缓冲区。 */
esp_err_t logs_clear_handler(WebServer::Request* request);
/** @brief 分页返回黑匣子记录。 */
esp_err_t blackbox_api_handler(WebServer::Request* request);
/** @brief 清空黑匣子记录。 */
esp_err_t blackbox_clear_handler(WebServer::Request* request);
/** @brief 更新黑匣子周期快照配置。 */
esp_err_t blackbox_config_handler(WebServer::Request* request);
/** @brief 扫描并返回附近 WiFi AP。 */
esp_err_t wifi_scan_handler(WebServer::Request* request);
/** @brief 保存凭据并连接 STA。 */
esp_err_t wifi_connect_handler(WebServer::Request* request);
/** @brief 切换到 AP 配网模式。 */
esp_err_t wifi_ap_handler(WebServer::Request* request);
/** @brief 停止 IP 网络并保留 ESP-NOW。 */
esp_err_t wifi_off_handler(WebServer::Request* request);
/** @brief 按持久化配置启动 WiFi/Web。 */
esp_err_t wifi_on_handler(WebServer::Request* request);
/** @brief 更新开机启用 WiFi/Web 的配置。 */
esp_err_t wifi_boot_handler(WebServer::Request* request);
/** @brief 清除已保存的 STA 凭据。 */
esp_err_t wifi_clear_handler(WebServer::Request* request);
/** @brief 开启 ESP-NOW 配对窗口。 */
esp_err_t espnow_pair_handler(WebServer::Request* request);
/** @brief 停止 ESP-NOW 配对。 */
esp_err_t espnow_pair_stop_handler(WebServer::Request* request);
/** @brief 清除全部 ESP-NOW peer。 */
esp_err_t espnow_pair_clear_handler(WebServer::Request* request);
/** @brief 返回本地和远端 OTA 状态。 */
esp_err_t ota_status_handler(WebServer::Request* request);
/** @brief 流式接收并校验上传固件。 */
esp_err_t ota_upload_handler(WebServer::Request* request);
/** @brief 激活已经校验的上传固件。 */
esp_err_t ota_activate_handler(WebServer::Request* request);
/** @brief 中止或放弃当前 OTA 会话。 */
esp_err_t ota_abort_handler(WebServer::Request* request);
/** @brief 异步检查远端固件版本。 */
esp_err_t ota_remote_check_handler(WebServer::Request* request);
/** @brief 异步下载并安装远端固件。 */
esp_err_t ota_remote_download_handler(WebServer::Request* request);

/** @brief 将 OTA APP 分区转换为用户可读槽位编号，未知时返回 0。 */
uint8_t ota_partition_slot(const esp_partition_t* partition);

} // namespace WebBackend

#endif

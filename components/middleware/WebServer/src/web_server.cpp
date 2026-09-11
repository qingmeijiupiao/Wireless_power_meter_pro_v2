/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 轻量级 HTTP WebServer 中间件实现
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-05-28
 */
#include "web_server.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace WebServer {

static const char* TAG = "WebServer";

struct Route {
    bool    used;
    char    uri[WEB_SERVER_URI_MAX_LEN];
    Method  method;
    Handler handler;
};

// HTTP服务器句柄与配置状态。当前实现假设路由在begin前注册完成。
static httpd_handle_t server_handle          = nullptr;
static uint16_t       server_port            = 80;
static bool           captive_portal_enabled = false;
static bool           initialized            = false;

// 路由、中间件和请求上下文均使用静态存储，避免HTTP任务栈被大buffer占用。
static Route      routes[WEB_SERVER_MAX_ROUTES];
static Middleware middlewares[WEB_SERVER_MAX_MIDDLEWARES];
static Request    active_request;
static uint8_t    route_count       = 0;
static uint8_t    middleware_count  = 0;
static Handler    not_found_handler = nullptr;

/**
 * @brief 清空路由表
 */
static void clear_routes() {
    for (uint8_t i = 0; i < WEB_SERVER_MAX_ROUTES; i++) {
        routes[i].used    = false;
        routes[i].uri[0]  = '\0';
        routes[i].method  = Method::GET;
        routes[i].handler = nullptr;
    }
    route_count = 0;
}

/**
 * @brief 清空中间件表
 */
static void clear_middlewares() {
    for (uint8_t i = 0; i < WEB_SERVER_MAX_MIDDLEWARES; i++) {
        middlewares[i] = nullptr;
    }
    middleware_count = 0;
}

/**
 * @brief 将模块内部方法转换为ESP-IDF方法枚举
 */
static httpd_method_t to_httpd_method(Method method) {
    switch (method) {
    case Method::GET:
        return HTTP_GET;
    case Method::POST:
        return HTTP_POST;
    case Method::PUT:
        return HTTP_PUT;
    case Method::DELETE_:
        return HTTP_DELETE;
    case Method::PATCH:
        return HTTP_PATCH;
    case Method::OPTIONS:
        return HTTP_OPTIONS;
    case Method::HEAD:
        return HTTP_HEAD;
    default:
        return HTTP_GET;
    }
}

/**
 * @brief 将ESP-IDF方法枚举转换为模块内部方法
 */
static Method from_httpd_method(httpd_method_t method) {
    switch (method) {
    case HTTP_GET:
        return Method::GET;
    case HTTP_POST:
        return Method::POST;
    case HTTP_PUT:
        return Method::PUT;
    case HTTP_DELETE:
        return Method::DELETE_;
    case HTTP_PATCH:
        return Method::PATCH;
    case HTTP_OPTIONS:
        return Method::OPTIONS;
    case HTTP_HEAD:
        return Method::HEAD;
    default:
        return Method::ANY;
    }
}

/**
 * @brief 将常用状态码转换为HTTP状态行
 */
static const char* status_to_string(int status_code) {
    switch (status_code) {
    case 200:
        return "200 OK";
    case 201:
        return "201 Created";
    case 202:
        return "202 Accepted";
    case 204:
        return "204 No Content";
    case 301:
        return "301 Moved Permanently";
    case 302:
        return "302 Found";
    case 400:
        return "400 Bad Request";
    case 401:
        return "401 Unauthorized";
    case 403:
        return "403 Forbidden";
    case 404:
        return "404 Not Found";
    case 405:
        return "405 Method Not Allowed";
    case 408:
        return "408 Request Timeout";
    case 409:
        return "409 Conflict";
    case 413:
        return "413 Payload Too Large";
    case 422:
        return "422 Unprocessable Content";
    case 500:
        return "500 Internal Server Error";
    case 501:
        return "501 Not Implemented";
    case 503:
        return "503 Service Unavailable";
    default:
        return "200 OK";
    }
}

/**
 * @brief 从URI中复制路径部分，丢弃query字符串
 */
static void copy_uri_without_query(const char* src, char* dst, size_t dst_size) {
    if (dst_size == 0) {
        return;
    }

    size_t i = 0;
    while (src[i] != '\0' && src[i] != '?' && i < dst_size - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void load_peer_ip(httpd_req_t* req, char* out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    strncpy(out, "unknown", out_size - 1);
    out[out_size - 1] = '\0';

    sockaddr_storage address     = {};
    socklen_t        address_len = sizeof(address);
    const int        socket_fd   = httpd_req_to_sockfd(req);
    if (socket_fd < 0 || getpeername(socket_fd, reinterpret_cast<sockaddr*>(&address), &address_len) != 0) {
        return;
    }
    if (address.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        inet_ntoa_r(ipv4->sin_addr, out, static_cast<int>(out_size));
    }
}

/**
 * @brief 路由匹配
 * @note  业务路由使用精确匹配，通配符只用于esp_http_server统一入口
 */
static bool uri_match(const char* route_uri, const char* request_uri) {
    return strcmp(route_uri, request_uri) == 0;
}

/**
 * @brief 查找匹配的路由项
 */
static Route* find_route(const char* uri, Method method) {
    for (uint8_t i = 0; i < route_count; i++) {
        if (!routes[i].used) {
            continue;
        }
        if (!uri_match(routes[i].uri, uri)) {
            continue;
        }
        if (routes[i].method == method || routes[i].method == Method::ANY) {
            return &routes[i];
        }
    }
    return nullptr;
}

esp_err_t load_body(Request* request) {
    if (request == nullptr || request->raw == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->body_loaded) {
        return ESP_OK;
    }

    size_t content_len = request->raw->content_len;
    if (content_len > WEB_SERVER_BODY_MAX_LEN) {
        // 请求体超过静态缓存上限，直接返回413，避免溢出。
        httpd_resp_send_err(request->raw, HTTPD_413_CONTENT_TOO_LARGE, "request body too large");
        return ESP_ERR_NO_MEM;
    }

    // httpd_req_recv可能因为超时返回，需要继续等待直到读完整个body。
    size_t received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(request->raw, request->body + received, content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        received += ret;
    }

    request->body[received] = '\0';
    request->body_len       = received;
    request->body_loaded    = true;
    return ESP_OK;
}

esp_err_t stream_body(Request* request, char* buffer, size_t buffer_size, BodyChunkHandler chunk_handler) {
    if (request == nullptr || request->raw == nullptr || buffer == nullptr || buffer_size == 0 ||
        chunk_handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->body_loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    constexpr uint8_t MAX_CONSECUTIVE_TIMEOUTS = 3;
    size_t            received                 = 0;
    uint8_t           consecutive_timeouts     = 0;
    while (received < request->raw->content_len) {
        size_t remaining  = request->raw->content_len - received;
        size_t chunk_size = remaining < buffer_size ? remaining : buffer_size;
        int    ret        = httpd_req_recv(request->raw, buffer, chunk_size);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++consecutive_timeouts >= MAX_CONSECUTIVE_TIMEOUTS) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }

        consecutive_timeouts = 0;
        esp_err_t err        = chunk_handler(buffer, static_cast<size_t>(ret));
        if (err != ESP_OK) {
            return err;
        }
        received += static_cast<size_t>(ret);
    }

    request->body_len    = received;
    request->body_loaded = true;
    return ESP_OK;
}

/**
 * @brief esp_http_server统一分发入口
 * @note  所有HTTP方法先进入这里，再由内部静态路由表分发
 */
static esp_err_t dispatch(httpd_req_t* req) {
    Request* request = &active_request;
    *request = {};
    request->raw    = req;
    request->method = from_httpd_method((httpd_method_t)req->method);
    copy_uri_without_query(req->uri, request->uri, sizeof(request->uri));
    load_peer_ip(req, request->peer_ip, sizeof(request->peer_ip));

    if (httpd_req_get_url_query_str(req, request->query, sizeof(request->query)) != ESP_OK) {
        request->query[0] = '\0';
    }

    for (uint8_t i = 0; i < middleware_count; i++) {
        if (middlewares[i] == nullptr) {
            continue;
        }
        esp_err_t ret = middlewares[i](request);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    // 先按URI和Method查找精确路由。
    Route* route = find_route(request->uri, request->method);
    if (route != nullptr && route->handler != nullptr) {
        return route->handler(request);
    }

    // Captive Portal 模式下，探测 URL 和其他未知地址只返回轻量重定向。
    // Android 会并发请求 generate_204；若直接给每个探测连接发送完整页面，
    // 探测客户端识别 Portal 后主动断开，服务端容易阻塞到发送超时。
    if (captive_portal_enabled) {
        return redirect(request, "/provision");
    }

    if (not_found_handler != nullptr) {
        return not_found_handler(request);
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
}

/**
 * @brief 为指定HTTP方法注册统一通配符入口
 */
static esp_err_t register_dispatcher(httpd_handle_t handle, Method method) {
    static const char uri_all[] = "/*";
    httpd_uri_t       uri       = {};
    uri.uri                     = uri_all;
    uri.method                  = to_httpd_method(method);
    uri.handler                 = dispatch;
    uri.user_ctx                = nullptr;
    return httpd_register_uri_handler(handle, &uri);
}

esp_err_t init(uint16_t port) {
    if (initialized) {
        return ESP_OK;
    }
    server_port = port;
    clear_routes();
    clear_middlewares();
    captive_portal_enabled = false;
    not_found_handler      = nullptr;
    initialized            = true;
    return ESP_OK;
}

esp_err_t begin() {
    if (!initialized) {
        init(server_port);
    }
    if (server_handle != nullptr) {
        return ESP_OK;
    }

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = server_port;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.max_uri_handlers = 8;
    // 请求上下文已移到静态区，按实测峰值保留约 3KB 余量覆盖日志和业务 handler 调用链。
    config.stack_size       = 6144;

    esp_err_t ret = httpd_start(&server_handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = register_dispatcher(server_handle, Method::GET);
    if (ret == ESP_OK)
        ret = register_dispatcher(server_handle, Method::POST);
    if (ret == ESP_OK)
        ret = register_dispatcher(server_handle, Method::PUT);
    if (ret == ESP_OK)
        ret = register_dispatcher(server_handle, Method::DELETE_);
    if (ret == ESP_OK)
        ret = register_dispatcher(server_handle, Method::PATCH);
    if (ret == ESP_OK)
        ret = register_dispatcher(server_handle, Method::OPTIONS);
    if (ret == ESP_OK)
        ret = register_dispatcher(server_handle, Method::HEAD);

    if (ret != ESP_OK) {
        httpd_stop(server_handle);
        server_handle = nullptr;
        ESP_LOGE(TAG, "register dispatcher failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "started on port %u", server_port);
    return ESP_OK;
}

esp_err_t stop() {
    if (server_handle == nullptr) {
        return ESP_OK;
    }
    esp_err_t ret = httpd_stop(server_handle);
    server_handle = nullptr;
    return ret;
}

esp_err_t deinit() {
    stop();
    initialized = false;
    clear_routes();
    clear_middlewares();
    not_found_handler = nullptr;
    return ESP_OK;
}

esp_err_t on(const char* uri, Method method, Handler handler) {
    if (uri == nullptr || handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (server_handle != nullptr) {
        ESP_LOGE(TAG, "route register after begin is not allowed");
        return ESP_ERR_INVALID_STATE;
    }

    // 重复注册同一路由时覆盖handler，便于组件初始化阶段调整。
    for (uint8_t i = 0; i < route_count; i++) {
        if (routes[i].used && routes[i].method == method && strcmp(routes[i].uri, uri) == 0) {
            routes[i].handler = handler;
            return ESP_OK;
        }
    }

    if (route_count >= WEB_SERVER_MAX_ROUTES) {
        ESP_LOGE(TAG, "route table full");
        return ESP_ERR_NO_MEM;
    }

    Route* route = &routes[route_count];
    route->used  = true;
    strncpy(route->uri, uri, sizeof(route->uri) - 1);
    route->uri[sizeof(route->uri) - 1] = '\0';
    route->method                      = method;
    route->handler                     = handler;
    route_count++;
    return ESP_OK;
}

esp_err_t use(Middleware middleware) {
    if (middleware == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (server_handle != nullptr) {
        ESP_LOGE(TAG, "middleware register after begin is not allowed");
        return ESP_ERR_INVALID_STATE;
    }
    if (middleware_count >= WEB_SERVER_MAX_MIDDLEWARES) {
        ESP_LOGE(TAG, "middleware table full");
        return ESP_ERR_NO_MEM;
    }
    middlewares[middleware_count++] = middleware;
    return ESP_OK;
}

void on_not_found(Handler handler) {
    if (server_handle != nullptr) {
        ESP_LOGW(TAG, "set not found handler after begin");
    }
    not_found_handler = handler;
}

void enable_captive_portal(bool enable) {
    captive_portal_enabled = enable;
}

bool is_running() {
    return server_handle != nullptr;
}

esp_err_t serve_static(const char* uri, const char* data, size_t size, const char* content_type) {
    if (uri == nullptr || data == nullptr || content_type == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return on(uri, Method::GET, [data, size, content_type](Request* request) -> esp_err_t {
        return send(request, 200, content_type, data, size);
    });
}

esp_err_t send(Request* request, int status_code, const char* content_type, const char* data, size_t size) {
    if (request == nullptr || request->raw == nullptr || content_type == nullptr || (data == nullptr && size > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_status(request->raw, status_to_string(status_code));
    httpd_resp_set_type(request->raw, content_type);
    return httpd_resp_send(request->raw, data == nullptr ? "" : data, size);
}

esp_err_t send_gzip(Request* request, int status_code, const char* content_type, const char* data, size_t size) {
    if (request == nullptr || request->raw == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_resp_set_hdr(request->raw, "Content-Encoding", "gzip");
    return send(request, status_code, content_type, data, size);
}

esp_err_t send_text(Request* request, const char* text) {
    if (text == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return send(request, 200, "text/plain", text, strlen(text));
}

esp_err_t send_html(Request* request, const char* html, size_t size) {
    if (html == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return send(request, 200, "text/html", html, size);
}

esp_err_t send_html_gzip(Request* request, const char* html, size_t size) {
    if (html == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_gzip(request, 200, "text/html", html, size);
}

esp_err_t send_json(Request* request, const char* json) {
    if (json == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return send(request, 200, "application/json", json, strlen(json));
}

esp_err_t redirect(Request* request, const char* location) {
    if (request == nullptr || request->raw == nullptr || location == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_resp_set_status(request->raw, "302 Found");
    httpd_resp_set_hdr(request->raw, "Location", location);
    httpd_resp_set_hdr(request->raw, "Cache-Control", "no-store");
    return httpd_resp_send(request->raw, "", 0);
}

esp_err_t get_header(Request* request, const char* key, char* value, size_t value_size) {
    if (request == nullptr || request->raw == nullptr || key == nullptr || value == nullptr || value_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return httpd_req_get_hdr_value_str(request->raw, key, value, value_size);
}

esp_err_t get_query_value(Request* request, const char* key, char* value, size_t value_size) {
    if (request == nullptr || key == nullptr || value == nullptr || value_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return httpd_query_key_value(request->query, key, value, value_size);
}

} // namespace WebServer

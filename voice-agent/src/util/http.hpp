// src/util/http.hpp
#pragma once
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

// 自包含的最小 HTTP 客户端（无 TLS，仅 http://）。
// 用于本地 SearXNG、自托管工具端点；https 在线 Provider 接入时
// 需换用带 TLS 的客户端（cpr/libcurl），接口保持不变。

class CancelToken;

namespace voice_agent::http {

// ========== URL 解析 ==========
struct Url {
    std::string scheme;   // "http"
    std::string host;     // "127.0.0.1"
    int         port{80};
    std::string path;     // "/search"
    std::string query;    // "q=..."
    std::string error;    // 非空表示解析失败

    bool ok() const { return scheme == "http" && !host.empty() && error.empty(); }
};

// 解析绝对 URL；仅支持 http://
Url parse_url(const std::string& url);

// 对查询串做 application/x-www-form-urlencoded 编码
std::string url_encode(const std::string& s);

// ========== 请求 / 响应 ==========
struct Request {
    std::string url;
    std::string method{"GET"};
    std::map<std::string, std::string> headers;  // 键名按原样发送
    int timeout_ms{5000};
    std::shared_ptr<CancelToken> cancel;         // 可空
};

struct Response {
    int                                     status{0};
    std::string                             body;
    std::map<std::string, std::string>      headers;  // 键被转为小写
    std::string                             error;    // 非空表示传输/超时/取消

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// 执行 GET 请求。网络错误/超时/取消不会抛异常，而是填入 Response::error。
Response get(const Request& req);

}  // namespace voice_agent::http
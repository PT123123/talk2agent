// src/util/http.hpp
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// 最小 HTTP 客户端。http:// 走自实现 socket，https:// 走系统 WinHTTP（Schannel TLS）。
//
// 为什么分两套：在线 Provider（OpenAI 兼容 API）必须 HTTPS，而拉 libcurl/CPR 进来会
// 显著增加部署负担。WinHTTP 是 Windows 自带，链接 winhttp.lib 即可。
//
// WinHTTP 还支持**流式读取**（SSE），这是接入在线 LLM 的硬需求 ——
// chat/completions 的流式响应要逐 token 到达，不能等整个 body 收完。

class CancelToken;

namespace voice_agent::http {

// ========== URL 解析 ==========
struct Url {
    std::string scheme;   // "http" / "https"
    std::string host;     // "api.openai.com"
    int         port{0};      // 0 = 按 scheme 取默认
    std::string path;     // "/v1/chat/completions"
    std::string query;    // "a=1&b=2"
    std::string error;    // 非空表示解析失败

    bool ok() const {
        return (scheme == "http" || scheme == "https") && !host.empty() &&
               error.empty();
    }
    int effective_port() const {
        return port ? port : (scheme == "https" ? 443 : 80);
    }
    bool is_tls() const { return scheme == "https"; }
    std::string host_port() const;
    std::string target() const;   // path + ("?" + query)
};

// 解析绝对 URL；支持 http:// 与 https://
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
    // 请求体（POST/PUT 用）。非空时 Content-Type 需自行在 headers 里给出。
    std::string body;
};

struct Response {
    int                                status{0};
    std::string                        body;
    std::map<std::string, std::string> headers;  // 键被转为小写
    std::string                        error;    // 非空表示传输/超时/取消

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// 执行 GET 请求。网络错误/超时/取消不会抛异常，而是填入 Response::error。
Response get(const Request& req);

// 执行 POST 请求。
Response post(const Request& req);

// ========== 流式请求 ==========
// 用于 SSE（Server-Sent Events）这类逐块到达的响应。
//
// 关键：on_chunk 在**收到数据时立即**被调用，而不是等整个 body 收完。
// 在线路由（online LLM）的流式输出完全依赖这一点 —— 等收完再播首字，
// 首响应延迟就从几百毫秒变成几秒。
//
// on_chunk 收到一个"数据块"（已剥离 "data: " 前缀，但可能不是完整一行）。
// 返回 false 表示消费方希望提前终止流。
using ChunkSink = std::function<bool(const std::string& chunk)>;

struct StreamRequest {
    Request base;   // url/method/headers/body/timeout/cancel
    ChunkSink on_chunk;
    // 收到完整响应头后调用（可选）。仅 2xx 时调用。
    std::function<void(int status)> on_headers;
};

struct StreamResponse {
    int         status{0};
    std::string error;
    // 累积的完整响应体。保留它是因为出错时需要看服务端返回了什么。
    std::string body;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

StreamResponse stream(const StreamRequest& req);

}  // namespace voice_agent::http

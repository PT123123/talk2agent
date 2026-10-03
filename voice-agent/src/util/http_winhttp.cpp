// src/util/http_winhttp.cpp
// WinHTTP（Windows 自带 / Schannel TLS）后端：负责 https:// 与流式 SSE。
//
// 为什么不拉 libcurl/CPR：在线 LLM API 必须要 HTTPS，而引入第三方 TLS 依赖会
// 显著增加 Windows 部署负担（windeployqt 还要带一批 DLL）。WinHTTP 是系统组件，
// 链接 winhttp.lib 即可，且原生支持"读到多少就回调多少"的流式读取。
//
// 本文件只在 _WIN32 下编译；非 Windows 平台回退到 http.cpp 的自实现 socket。

#include "http.hpp"
#include "core/cancel_token.hpp"
#include "util/log.hpp"

#ifdef _WIN32

#ifdef _MSC_VER
#  pragma comment(lib, "winhttp.lib")
#  pragma comment(lib, "advapi32.lib")
#endif

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>

namespace voice_agent::http {
namespace winhttp_detail {

namespace {

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string wstr_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                        static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wstr(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          out.data(), n);
    return out;
}

// RAII 请求句柄
struct RequestHandle {
    HINTERNET h{nullptr};
    ~RequestHandle() { if (h) ::WinHttpCloseHandle(h); }
};

std::string last_error_string(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string out;
    if (n && buf) {
        out = wstr_to_utf8(std::wstring(buf, n));
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
            out.pop_back();
        }
    }
    if (buf) ::LocalFree(buf);
    if (out.empty()) out = "winhttp error " + std::to_string(code);
    return out;
}

// 用给定标志打开请求。https 时自动补 WINHTTP_FLAG_SECURE。
bool open_request(RequestHandle& req, HINTERNET connect, const Url& u,
                  const std::string& method, DWORD flags, std::string& err) {
    std::wstring m = utf8_to_wstr(method);
    std::wstring target = utf8_to_wstr(u.target());
    DWORD f = flags;
    if (u.is_tls()) f |= WINHTTP_FLAG_SECURE;
    req.h = ::WinHttpOpenRequest(connect, m.c_str(), target.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 f);
    if (!req.h) {
        err = last_error_string(::GetLastError());
        return false;
    }
    return true;
}

std::wstring header_name_to_wstr(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) {
        // WinHTTP 要求 HTTP 头名以 \r\n 结束
        w.push_back(static_cast<unsigned char>(c));
        w.push_back(L'\r');
        w.push_back(L'\n');
    }
    return w;
}

std::wstring header_value_to_wstr(const std::string& s) {
    std::wstring w = utf8_to_wstr(s);
    w.push_back(L'\r');
    w.push_back(L'\n');
    return w;
}

bool send_headers(RequestHandle& req, const std::map<std::string, std::string>& headers,
                  const std::string& body, bool has_body, std::string& err) {
    std::wstring hdr;
    for (const auto& [k, v] : headers) {
        hdr += header_name_to_wstr(k);
        hdr += header_value_to_wstr(v);
    }
    hdr.push_back(L'\0');   // 双 NUL 终止
    hdr.pop_back();

    const BOOL ok = ::WinHttpSendRequest(
        req.h, hdr.c_str(), static_cast<DWORD>(hdr.size()),
        has_body ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA,
        has_body ? static_cast<DWORD>(body.size()) : 0,
        static_cast<DWORD>(body.size()), 0);
    if (!ok) {
        err = last_error_string(::GetLastError());
        return false;
    }
    return true;
}

bool receive_response(RequestHandle& req, int& status, std::string& err) {
    if (!::WinHttpReceiveResponse(req.h, nullptr)) {
        err = last_error_string(::GetLastError());
        return false;
    }
    DWORD code = 0;
    DWORD sz = sizeof(code);
    if (!::WinHttpQueryHeaders(req.h,
                               WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                               WINHTTP_NO_HEADER_INDEX)) {
        err = last_error_string(::GetLastError());
        return false;
    }
    status = static_cast<int>(code);
    return true;
}

// 从 chunk 缓冲里取出完整的 SSE 行（以 \n 结尾）。"data: " 前缀已剥离。
// 留空则表示本次没有完整行。
std::string take_sse_line(std::string& buf) {
    size_t pos = buf.find('\n');
    if (pos == std::string::npos) return {};
    std::string line = buf.substr(0, pos);
    buf.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

}  // namespace

// ========== 会话池 ==========
// 每轮新建 WinHTTP session 的开销直接叠在 TTFT 上（TCP 握手 + TLS 握手
// 动辄 100~300ms）。WinHTTP 的 session/connect 本身是可复用的
// （内部会做 keep-alive），所以按 host:port 缓存一对句柄。
//
// 边界：
//   - 只缓存 https（http:// 走自实现 socket，不涉及）
//   - 句柄线程安全？WinHTTP session 默认可跨线程用，但同一 session 上
//     并发发请求需要各自的 request 句柄（本代码已如此）。这里只缓存
//     session+connect，请求句柄仍是每次新建 —— 这是正确的用法。
//   - 进程退出时由 static 析构统一关闭。
class SessionPool {
public:
    struct Entry {
        HINTERNET session{nullptr};
        HINTERNET connect{nullptr};
    };

    static SessionPool& instance() {
        static SessionPool p;
        return p;
    }

    // 取（必要时建立）host:port 的会话对。失败返回 false 并填 err。
    bool acquire(const std::string& host_port, int timeout_ms, Entry& out,
                 std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(host_port);
        if (it != cache_.end()) {
            out = it->second;
            return true;
        }

        Entry e;
        // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 会读系统代理设置，
        // 在有代理的企业环境里这是必须的（直连会失败）。
        e.session = ::WinHttpOpen(L"voice-agent/1.0",
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!e.session) {
            err = last_error_string(::GetLastError());
            return false;
        }
        const DWORD t = static_cast<DWORD>(timeout_ms > 0 ? timeout_ms : 15000);
        ::WinHttpSetTimeouts(e.session, static_cast<int>(t), static_cast<int>(t),
                             static_cast<int>(t), static_cast<int>(t));

        // 拆 host:port（IPv6 字面量带方括号，不做完整解析，够用）
        std::string host = host_port;
        int port = 443;
        auto colon = host_port.rfind(':');
        if (colon != std::string::npos) {
            host = host_port.substr(0, colon);
            port = std::atoi(host_port.c_str() + colon + 1);
        }
        if (!host.empty() && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }

        e.connect = ::WinHttpConnect(e.session, utf8_to_wstr(host).c_str(),
                                     static_cast<INTERNET_PORT>(port), 0);
        if (!e.connect) {
            err = last_error_string(::GetLastError());
            ::WinHttpCloseHandle(e.session);
            return false;
        }
        cache_[host_port] = e;
        out = e;
        return true;
    }

    ~SessionPool() {
        for (auto& [k, e] : cache_) {
            if (e.connect) ::WinHttpCloseHandle(e.connect);
            if (e.session) ::WinHttpCloseHandle(e.session);
        }
    }

private:
    std::mutex mutex_;
    std::map<std::string, Entry> cache_;
};

// ========== 非流式请求 ==========
Response do_request(const Request& req) {
    Response resp;

    Url u = parse_url(req.url);
    if (!u.ok()) {
        resp.error = u.error.empty() ? "bad url" : u.error;
        return resp;
    }

    std::string err;
    SessionPool::Entry shared;
    if (!SessionPool::instance().acquire(u.host_port(), req.timeout_ms, shared, err)) {
        resp.error = "session: " + err;
        return resp;
    }
    // 池里的句柄由池持有，这里借用，不做 RAII 关闭。
    RequestHandle rh;
    if (!open_request(rh, shared.connect, u, req.method, 0, err)) {
        resp.error = "request: " + err;
        return resp;
    }

    const bool has_body = !req.body.empty();
    if (!send_headers(rh, req.headers, req.body, has_body, err)) {
        resp.error = "send: " + err;
        return resp;
    }
    if (req.cancel && req.cancel->cancelled()) {
        resp.error = "cancelled";
        return resp;
    }
    if (!receive_response(rh, resp.status, err)) {
        resp.error = "receive: " + err;
        return resp;
    }

    // 读完整 body
    std::string body;
    char buf[8192];
    for (;;) {
        DWORD read = 0;
        if (!::WinHttpReadData(rh.h, buf, sizeof(buf), &read)) {
            err = last_error_string(::GetLastError());
            // 收到部分数据后出错：保留已得内容，但标记失败
            resp.body = std::move(body);
            resp.error = "read: " + err;
            return resp;
        }
        if (read == 0) break;
        body.append(buf, read);

        if (req.cancel && req.cancel->cancelled()) {
            resp.body = std::move(body);
            resp.error = "cancelled";
            return resp;
        }
    }
    resp.body = std::move(body);
    return resp;
}

// ========== 流式请求 ==========
StreamResponse do_stream(const StreamRequest& sreq) {
    StreamResponse sresp;

    Url u = parse_url(sreq.base.url);
    if (!u.ok()) {
        sresp.error = u.error.empty() ? "bad url" : u.error;
        return sresp;
    }

    std::string err;
    SessionPool::Entry shared;
    if (!SessionPool::instance().acquire(u.host_port(), sreq.base.timeout_ms, shared,
                                         err)) {
        sresp.error = "session: " + err;
        return sresp;
    }
    RequestHandle rh;
    // 关键：不能用任何"收完整个 body"的标志（如 ENABLE_READING），
    // 否则 WinHttpReadData 会阻塞到 body 收完，流式就没了。
    if (!open_request(rh, shared.connect, u, sreq.base.method, 0, err)) {
        sresp.error = "request: " + err;
        return sresp;
    }
    const bool has_body = !sreq.base.body.empty();
    if (!send_headers(rh, sreq.base.headers, sreq.base.body, has_body, err)) {
        sresp.error = "send: " + err;
        return sresp;
    }
    if (sreq.base.cancel && sreq.base.cancel->cancelled()) {
        sresp.error = "cancelled";
        return sresp;
    }
    if (!receive_response(rh, sresp.status, err)) {
        sresp.error = "receive: " + err;
        return sresp;
    }

    if (sresp.status < 200 || sresp.status >= 300) {
        // 非 2xx：把错误 body 读出来，服务端的报错信息比状态码有用得多
        char buf[4096];
        DWORD read = 0;
        while (::WinHttpReadData(rh.h, buf, sizeof(buf), &read) && read > 0) {
            sresp.body.append(buf, read);
            if (sresp.body.size() > 16384) break;   // 够看报错即可
        }
        sresp.error = "http " + std::to_string(sresp.status);
        return sresp;
    }

    if (sreq.on_headers) sreq.on_headers(sresp.status);

    // 逐块读取。WinHttpQueryDataAvailable 告诉我们有多少字节可读，
    // 读多少就立刻回调多少 —— 这是流式低延迟的关键。
    std::string line_buf;
    char buf[4096];
    bool consumer_stopped = false;

    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(rh.h, &avail)) {
            err = last_error_string(::GetLastError());
            sresp.error = "query available: " + err;
            break;
        }
        if (avail == 0) break;

        DWORD to_read = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
        DWORD read = 0;
        if (!::WinHttpReadData(rh.h, buf, to_read, &read)) {
            err = last_error_string(::GetLastError());
            sresp.error = "read: " + err;
            break;
        }
        if (read == 0) break;

        sresp.body.append(buf, read);
        if (sreq.base.cancel && sreq.base.cancel->cancelled()) {
            sresp.error = "cancelled";
            break;
        }

        // 剥离 SSE 帧：逐行处理，"data: " 前缀去掉，
        // "[DONE]" 之类保留给上层判断（这里不吞，因为上层需要知道结束）。
        if (sreq.on_chunk) {
            line_buf.append(buf, read);
            for (;;) {
                std::string line = take_sse_line(line_buf);
                if (line.empty()) break;
                // 空行是 SSE 的帧分隔，跳过
                if (line.empty()) continue;
                if (line.rfind("data:", 0) == 0) {
                    std::string data = line.substr(5);
                    if (!data.empty() && data[0] == ' ') data.erase(0, 1);
                    if (data.empty()) continue;
                    if (!sreq.on_chunk(data)) {
                        consumer_stopped = true;
                        break;
                    }
                } else if (line[0] == ':') {
                    // SSE 注释行（心跳），忽略
                    continue;
                } else {
                    // 非 SSE 帧（服务端返回了普通 JSON 等）：整行交给上层
                    if (!sreq.on_chunk(line)) {
                        consumer_stopped = true;
                        break;
                    }
                }
            }
            if (consumer_stopped) break;
        }
    }

    // 消费方提前停止不算错误（它自己决定不听了）
    if (consumer_stopped && sresp.error.empty()) {
        return sresp;
    }
    return sresp;
}

}  // namespace winhttp_detail
}  // namespace voice_agent::http

#endif  // _WIN32

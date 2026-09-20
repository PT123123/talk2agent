// src/util/http.cpp
#include "http.hpp"
#include "core/cancel_token.hpp"

#ifdef _WIN32
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <cerrno>
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace voice_agent::http {

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kInvalid = INVALID_SOCKET;
#else
using sock_t = int;
constexpr sock_t kInvalid = -1;
#endif

void close_sock(sock_t s) {
#ifdef _WIN32
    if (s != kInvalid) closesocket(s);
#else
    if (s != kInvalid) ::close(s);
#endif
}

// Socket 可选（非阻塞）标记
void set_nonblocking(sock_t s, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return;
    if (nb) flags |= O_NONBLOCK;
    else    flags &= ~O_NONBLOCK;
    fcntl(s, F_SETFL, flags);
#endif
}

// 使用 select() 等待 socket 可读，带超时；返回 false 表示超时/出错/取消
bool wait_readable(sock_t s, int timeout_ms, const std::shared_ptr<CancelToken>& ct) {
    struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);

    if (ct) {
        // 取消感知：轮询而不是盲目阻塞整个超时
        constexpr int slice = 50;  // ms
        int elapsed = 0;
        while (elapsed < timeout_ms) {
            if (ct->cancelled()) return false;
            struct timeval stv{0, slice * 1000};
            fd_set srfds = rfds;
            int r = select(static_cast<int>(s) + 1, &srfds, nullptr, nullptr, &stv);
            if (r > 0) return true;
            elapsed += slice;
        }
        return false;
    }

    int r = select(static_cast<int>(s) + 1, &rfds, nullptr, nullptr, &tv);
    return r > 0;
}

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::map<std::string, std::string> parse_headers(const std::vector<std::string>& lines, int from) {
    std::map<std::string, std::string> out;
    for (size_t i = from; i < lines.size(); ++i) {
        const std::string& ln = lines[i];
        if (ln.empty()) break;
        auto colon = ln.find(':');
        if (colon == std::string::npos) continue;
        std::string key = lowercase(ln.substr(0, colon));
        std::string val = ln.substr(colon + 1);
        // 去掉首尾空白
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
        out[key] = val;
    }
    return out;
}

// 分块传输解码
std::string decode_chunked(const std::string& body) {
    std::string out;
    out.reserve(body.size());
    size_t i = 0;
    while (i < body.size()) {
        auto cr = body.find("\r\n", i);
        if (cr == std::string::npos) break;
        size_t len = 0;
        auto res = std::from_chars(body.data() + i, body.data() + cr, len, 16);
        if (res.ec != std::errc()) break;
        i = cr + 2;
        if (len == 0) break;  // 终止块
        if (i + len > body.size()) break;
        out.append(body, i, len);
        i += len;
        if (i + 2 <= body.size() && body.compare(i, 2, "\r\n") == 0) i += 2;
    }
    return out;
}

}  // namespace

// ========== URL 解析 ==========

Url parse_url(const std::string& url) {
    Url u;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        u.error = "missing scheme";
        return u;
    }
    u.scheme = lowercase(url.substr(0, scheme_end));
    if (u.scheme != "http") {
        u.error = "unsupported scheme '" + u.scheme + "' (only http://)";
        return u;
    }

    size_t p = scheme_end + 3;
    auto host_end = url.find_first_of("/?#", p);
    std::string authority = (host_end == std::string::npos) ? url.substr(p) : url.substr(p, host_end - p);

    u.port = 80;
    auto colon = authority.find(':');
    if (colon != std::string::npos) {
        u.host = authority.substr(0, colon);
        std::string port_str = authority.substr(colon + 1);
        int port = 0;
        auto r = std::from_chars(port_str.data(), port_str.data() + port_str.size(), port);
        if (r.ec == std::errc() && r.ptr == port_str.data() + port_str.size())
            u.port = port;
        else {
            u.error = "invalid port";
            return u;
        }
    } else {
        u.host = authority;
    }

    if (u.host.empty()) {
        u.error = "empty host";
        return u;
    }

    if (host_end != std::string::npos && url[host_end] == '/') {
        auto path_end = url.find_first_of("?#", host_end + 1);
        u.path = (path_end == std::string::npos) ? url.substr(host_end)
                                                : url.substr(host_end, path_end - host_end);
        p = (path_end == std::string::npos) ? url.size() : path_end;
    } else {
        u.path = "/";
        p = host_end == std::string::npos ? url.size() : host_end;
    }

    if (p < url.size() && url[p] == '?') {
        u.query = url.substr(p + 1);
    }
    return u;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

// ========== HTTP GET ==========

Response get(const Request& req) {
    Response resp;
    Url u = parse_url(req.url);
    if (!u.ok()) {
        resp.error = "invalid URL: " + u.error;
        return resp;
    }

#ifdef _WIN32
    static bool ws_initialized = false;
    {
        static WSADATA wsa;
        if (!ws_initialized) {
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                resp.error = "WSAStartup failed";
                return resp;
            }
            ws_initialized = true;
        }
    }
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addrs = nullptr;
    std::string port_str = std::to_string(u.port);
    int ga = getaddrinfo(u.host.c_str(), port_str.c_str(), &hints, &addrs);
    if (ga != 0) {
        resp.error = "getaddrinfo failed: " + std::string(gai_strerror(ga));
        return resp;
    }

    sock_t sock = kInvalid;
    for (addrinfo* ai = addrs; ai != nullptr; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == kInvalid) continue;

        // 非阻塞 connect + select 实现超时
        set_nonblocking(sock, true);
        int cr = connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (cr != 0) {
            bool pending = false;
#ifdef _WIN32
            pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            pending = (errno == EINPROGRESS) || (errno == EWOULDBLOCK);
#endif
            if (pending) {
                // 等待可写
                struct timeval tv{req.timeout_ms / 1000, (req.timeout_ms % 1000) * 1000};
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(sock, &wfds);
                int sr = select(static_cast<int>(sock) + 1, nullptr, &wfds, nullptr, &tv);
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
                if (sr <= 0 || so_error != 0) {
                    close_sock(sock);
                    sock = kInvalid;
                    continue;
                }
            } else {
                close_sock(sock);
                sock = kInvalid;
                continue;
            }
        }
        set_nonblocking(sock, false);
        break;
    }
    freeaddrinfo(addrs);

    if (sock == kInvalid) {
        resp.error = "TCP connect failed / timeout";
        return resp;
    }

    // 构造请求
    std::ostringstream oss;
    oss << "GET " << u.path;
    if (!u.query.empty()) oss << "?" << u.query;
    oss << " HTTP/1.1\r\n"
        << "Host: " << u.host << (u.port != 80 ? ":" + std::to_string(u.port) : "") << "\r\n"
        << "User-Agent: voice-agent/0.1\r\n"
        << "Accept: */*\r\n"
        << "Connection: close\r\n";
    for (auto& [k, v] : req.headers)
        oss << k << ": " << v << "\r\n";
    oss << "\r\n";
    std::string head = oss.str();

    // 发送（阻塞写）
    size_t sent = 0;
    while (sent < head.size()) {
        int n = send(sock, head.data() + sent, static_cast<int>(head.size() - sent), 0);
        if (n <= 0) {
            resp.error = "send failed";
            close_sock(sock);
            return resp;
        }
        sent += static_cast<size_t>(n);
    }

    // 读取响应
    std::string raw;
    char buf[16384];
    int64_t deadline_ms = req.timeout_ms;
    while (true) {
        if (req.cancel && req.cancel->cancelled()) {
            resp.error = "cancelled";
            close_sock(sock);
            return resp;
        }
        if (deadline_ms <= 0) {
            resp.error = "read timeout";
            close_sock(sock);
            return resp;
        }
        if (!wait_readable(sock, static_cast<int>(std::min<int64_t>(deadline_ms, 2000)),
                           req.cancel)) {
            if (req.cancel && req.cancel->cancelled()) {
                resp.error = "cancelled";
                close_sock(sock);
                return resp;
            }
            deadline_ms -= 2000;
            continue;
        }
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n == 0) break;  // 连接关闭
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                deadline_ms -= 50;
                continue;
            }
#else
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                deadline_ms -= 50;
                continue;
            }
#endif
            resp.error = "recv failed";
            close_sock(sock);
            return resp;
        }
        raw.append(buf, static_cast<size_t>(n));
    }
    close_sock(sock);

    if (raw.empty()) {
        resp.error = "empty response";
        return resp;
    }

    // 解析状态行 + 头 + 体
    auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        resp.error = "malformed response";
        return resp;
    }
    std::string head_part = raw.substr(0, header_end);
    std::string body_part = raw.substr(header_end + 4);

    // 用简单的 split 处理换行
    std::vector<std::string> lines;
    {
        std::istringstream hss(head_part);
        std::string ln;
        while (std::getline(hss, ln)) lines.push_back(ln);
    }
    if (lines.empty()) {
        resp.error = "malformed status line";
        return resp;
    }

    int status = 0;
    {
        auto sp1 = lines[0].find(' ');
        if (sp1 != std::string::npos) {
            auto sp2 = lines[0].find(' ', sp1 + 1);
            std::string code_str = lines[0].substr(sp1 + 1, sp2 - sp1 - 1);
            auto r = std::from_chars(code_str.data(), code_str.data() + code_str.size(), status);
            if (r.ec != std::errc()) status = 0;
        }
    }
    resp.status = status;
    resp.headers = parse_headers(lines, 1);

    std::string body = body_part;
    auto it = resp.headers.find("transfer-encoding");
    if (it != resp.headers.end() &&
        lowercase(it->second).find("chunked") != std::string::npos) {
        body = decode_chunked(body_part);
    }
    resp.body = std::move(body);
    return resp;
}

}  // namespace voice_agent::http
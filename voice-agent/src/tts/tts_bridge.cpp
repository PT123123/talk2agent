// src/tts/tts_bridge.cpp
#include "tts/tts_bridge.hpp"

#include "util/http.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <utility>

namespace voice_agent {

using json = nlohmann::json;

namespace {

std::string trim_trailing_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// WAV chunk 遍历：RIFF(4) size(4) WAVE(4) 然后是若干 chunk。
// 每个 chunk: id(4) size(4) data(size)，size 为奇数时有 1 字节 pad（规范如此，
// 实际 WAV 少见，但按规范处理才不会在某些导出器上错位）。
struct WavFmt {
    int audio_format{0};
    int channels{0};
    int sample_rate{0};
    int bits{0};
};

// 小端读取。调用方已保证指针在缓冲区内，故不做额外边界检查 —
// 越界读是这里的真正风险，所以每个调用点前面都有 body+csize<=n 的判断。
int read_le16(const unsigned char* p) {
    return static_cast<int>(p[0] | (p[1] << 8));
}

int read_le32(const unsigned char* p) {
    return static_cast<int>(p[0] | (p[1] << 8) | (p[2] << 16) |
                            (static_cast<unsigned>(p[3]) << 24));
}

}  // namespace

// ========== 引擎枚举 ==========

TtsBridgeEngine tts_bridge_engine_from_string(const std::string& s) {
    if (s == "qwen3tts") return TtsBridgeEngine::Qwen3Tts;
    if (s == "chatterbox") return TtsBridgeEngine::Chatterbox;
    return TtsBridgeEngine::Unknown;
}

const char* tts_bridge_engine_name(TtsBridgeEngine e) {
    switch (e) {
        case TtsBridgeEngine::Qwen3Tts: return "qwen3tts";
        case TtsBridgeEngine::Chatterbox: return "chatterbox";
        case TtsBridgeEngine::Unknown: break;
    }
    return "unknown";
}

// ========== 纯函数 ==========

bool parse_wav_pcm16(const std::string& wav, std::vector<int16_t>& out,
                     int& sample_rate, int& channels) {
    out.clear();
    sample_rate = 0;
    channels = 0;

    const auto* p = reinterpret_cast<const unsigned char*>(wav.data());
    const size_t n = wav.size();
    if (n < 44) return false;
    if (std::memcmp(p, "RIFF", 4) != 0) return false;
    if (std::memcmp(p + 8, "WAVE", 4) != 0) return false;

    WavFmt fmt;
    const char* data_ptr = nullptr;
    size_t data_size = 0;

    size_t pos = 12;
    while (pos + 8 <= n) {
        char id[5] = {0};
        std::memcpy(id, p + pos, 4);
        const int csize = read_le32(p + pos + 4);
        if (csize < 0) return false;
        const size_t body = pos + 8;
        if (body + static_cast<size_t>(csize) > n) return false;

        if (std::memcmp(id, "fmt ", 4) == 0 && csize >= 16) {
            fmt.audio_format = read_le16(p + body);
            fmt.channels     = read_le16(p + body + 2);
            fmt.sample_rate  = read_le32(p + body + 4);
            fmt.bits         = read_le16(p + body + 14);
            // 0xFFFE = WAVE_FORMAT_EXTENSIBLE，实际按 PCM 处理；
            // bridge 侧本来就是 PCM_16，不会有真扩展格式。
            if (fmt.audio_format != 1 && fmt.audio_format != 0xFFFE) return false;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_ptr = reinterpret_cast<const char*>(p + body);
            data_size = static_cast<size_t>(csize);
        }

        pos = body + static_cast<size_t>(csize);
        if (csize & 1) ++pos;   // pad byte
    }

    if (!data_ptr || data_size < 2) return false;
    if (fmt.bits != 16) return false;   // bridge 固定 PCM_16，其它格式不做猜测
    if (fmt.channels < 1) return false;

    sample_rate = fmt.sample_rate;
    channels = fmt.channels;

    const size_t count = data_size / 2;
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* s = reinterpret_cast<const unsigned char*>(
            data_ptr + i * 2);
        out[i] = static_cast<int16_t>(static_cast<uint16_t>(
            static_cast<unsigned>(s[0] | (s[1] << 8))));
    }

    // 多声道取第一声道：voice-agent 播放链路是单声道，
    // 混成单声道会让语速听感变化（多声道相位差叠加），取首声道最稳。
    if (fmt.channels > 1) {
        const size_t frames = count / static_cast<size_t>(fmt.channels);
        out.resize(frames);
    }
    return !out.empty();
}

std::vector<int16_t> resample_linear(const std::vector<int16_t>& in,
                                     int src_rate, int dst_rate) {
    if (in.empty() || src_rate == dst_rate || src_rate <= 0 || dst_rate <= 0)
        return in;
    const double ratio = static_cast<double>(dst_rate) / src_rate;
    const size_t n_out = static_cast<size_t>(std::llround(in.size() * ratio));
    if (n_out == 0) return {};

    std::vector<int16_t> out(n_out);
    const double step = static_cast<double>(in.size()) / n_out;
    for (size_t i = 0; i < n_out; ++i) {
        const double x = i * step;
        const size_t i0 = static_cast<size_t>(x);
        const size_t i1 = std::min(i0 + 1, in.size() - 1);
        const double frac = x - static_cast<double>(i0);
        // 用 double 算再钳制：直接 int 插值会累积量化误差，长音频末尾会漂。
        const double v = in[i0] * (1.0 - frac) + in[i1] * frac;
        out[i] = static_cast<int16_t>(
            std::clamp(v, -32768.0, 32767.0));
    }
    return out;
}

std::string build_synthesize_body(const std::string& text,
                                  const TtsBridgeControls& c, int sample_rate) {
    // 两个引擎的控制字段都带上，由 Python 端按 engine 决定消费哪些。
    // 这样 C++ 侧不需要"哪个字段对哪个引擎有效"的知识 ——
    // 那份知识在 Python 端（与该引擎的 SDK 调用签名最近）。
    json j;
    j["text"] = text;
    j["instruction"] = c.instruction;
    j["voice"] = c.voice;
    j["speed"] = c.speed;
    j["exaggeration"] = c.exaggeration;
    j["cfg_weight"] = c.cfg_weight;
    j["lang"] = c.lang;
    j["sample_rate"] = sample_rate;
    return j.dump();
}

const char* tts_bridge_health_name(TtsBridgeHealth h) {
    switch (h) {
        case TtsBridgeHealth::Ready: return "ready";
        case TtsBridgeHealth::Loading: return "loading";
        case TtsBridgeHealth::Unavailable: break;
    }
    return "unavailable";
}

TtsBridgeHealth parse_health_state(const std::string& body, std::string* detail) {
    auto give = [&](TtsBridgeHealth h) {
        if (detail) *detail = "backend_state missing";
        return h;
    };
    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& e) {
        if (detail) *detail = std::string("bad json: ") + e.what();
        return TtsBridgeHealth::Unavailable;
    }
    if (!j.contains("ok") || !j["ok"].is_boolean() || !j["ok"].get<bool>()) {
        if (detail) *detail = "ok != true";
        return TtsBridgeHealth::Unavailable;
    }

    std::string state;
    if (j.contains("backend_state") && j["backend_state"].is_string())
        state = j["backend_state"].get<std::string>();
    if (detail && j.contains("detail") && j["detail"].is_string())
        *detail = j["detail"].get<std::string>();

    // backend_state 是权威（新版 bridge 提供）。老版本只有 backend_ready，
    // 照样能判：true=ready / false=loading 或 unavailable 分不清，
    // 按 unavailable 处理 —— 那正是老版本的行为（还没加载完）。
    if (state == "ready")
        return TtsBridgeHealth::Ready;
    if (state == "loading")
        return TtsBridgeHealth::Loading;
    if (state == "error")
        return TtsBridgeHealth::Unavailable;
    if (state.empty()) {
        const bool ready = j.contains("backend_ready") &&
                           j["backend_ready"].is_boolean() &&
                           j["backend_ready"].get<bool>();
        return ready ? TtsBridgeHealth::Ready : TtsBridgeHealth::Unavailable;
    }
    return give(TtsBridgeHealth::Unavailable);
}

// ========== 客户端 ==========

TtsBridge::TtsBridge(TtsBridgeConfig cfg) : cfg_(std::move(cfg)) {
    cfg_.endpoint = trim_trailing_slash(cfg_.endpoint);
}

TtsBridge::~TtsBridge() = default;

TtsBridgeHealth TtsBridge::probe(std::string* detail, int wait_budget_ms) {
    const auto once = [&]() {
        http::Request req;
        req.url = cfg_.endpoint + "/health";
        req.method = "GET";
        req.timeout_ms = cfg_.health_timeout_ms;
        return http::get(req);
    };

    auto resp = once();
    if (!resp.error.empty() || resp.status != 200) {
        health_ = TtsBridgeHealth::Unavailable;
        if (detail) {
            *detail = "bridge unreachable at " + cfg_.endpoint + ": " +
                      (resp.error.empty()
                           ? "HTTP " + std::to_string(resp.status)
                           : resp.error);
        }
        return health_;
    }

    health_ = parse_health_state(resp.body, detail);

    // Loading：后端正在加载权重。这不是失败，值得等—— 但只在调用方
    // 明确给了预算时等（启动路径通常给几百毫秒，交互路径不给）。
    // 每轮对话都等几十秒是不能接受的。
    if (health_ == TtsBridgeHealth::Loading && wait_budget_ms > 0) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(wait_budget_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            auto again = once();
            if (!again.error.empty() || again.status != 200) break;
            health_ = parse_health_state(again.body, detail);
            if (health_ != TtsBridgeHealth::Loading) break;
        }
        if (health_ == TtsBridgeHealth::Loading && detail) {
            *detail += " (still loading after wait budget)";
        }
    }
    return health_;
}

bool TtsBridge::synthesize(const std::string& text,
                           const TtsBridgeControls& controls,
                           std::vector<int16_t>& out, std::string* err) {
    out.clear();
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        LOG_WARN("TtsBridge({}): {}", tts_bridge_engine_name(cfg_.engine), why);
        return false;
    };

    if (text.empty()) return fail("empty text");

    http::Request req;
    req.url = cfg_.endpoint + "/synthesize";
    req.method = "POST";
    req.timeout_ms = cfg_.timeout_ms;
    req.headers["Content-Type"] = "application/json";
    req.body = build_synthesize_body(text, controls, cfg_.sample_rate);
    req.cancel = cancel_;

    auto resp = http::post(req);

    if (!resp.error.empty()) return fail("bridge transport: " + resp.error);

    if (resp.status == 409) {
        // bridge 明确说"被barge-in 打断了"。这不是错误，上层静默丢弃本段。
        if (err) err->clear();
        LOG_DEBUG("TtsBridge: interrupted by barge-in");
        return false;
    }
    if (resp.status != 200) {
        // 错误 body 是 JSON {"error": ...}，比状态码有用得多。
        std::string why = "bridge HTTP " + std::to_string(resp.status);
        try {
            json j = json::parse(resp.body);
            if (j.contains("error") && j["error"].is_string())
                why += ": " + j["error"].get<std::string>();
        } catch (const std::exception&) {
            if (resp.body.size() < 200) why += ": " + resp.body;
        }
        // 后端不可用（缺包/没权重）时永久标记，避免每段都白等一次超时。
        if (resp.status == 503) health_ = TtsBridgeHealth::Unavailable;
        return fail(why);
    }

    int sr = 0, ch = 0;
    if (!parse_wav_pcm16(resp.body, out, sr, ch))
        return fail("failed to parse WAV from bridge (" +
                    std::to_string(resp.body.size()) + " bytes)");

    if (sr != cfg_.sample_rate) {
        out = resample_linear(out, sr, cfg_.sample_rate);
    }
    if (out.empty()) return fail("bridge returned empty audio");

    LOG_INFO("TtsBridge({}): '{}' -> {} samples @ {} Hz", 
             tts_bridge_engine_name(cfg_.engine), text, out.size(),
             cfg_.sample_rate);
    return true;
}

void TtsBridge::interrupt() {
    http::Request req;
    req.url = cfg_.endpoint + "/interrupt";
    req.method = "POST";
    req.timeout_ms = 300;      // fire-and-forget：宁可失败也不能拖住打断路径
    req.headers["Content-Type"] = "application/json";
    req.body = "{}";
    (void)http::post(req);
}

}  // namespace voice_agent
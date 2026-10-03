// src/orchestrator/remote_llm.cpp
#include "remote_llm.hpp"
#include "util/http.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>

namespace voice_agent {

using json = nlohmann::json;
using namespace std::chrono;

namespace {

double now_ms() {
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string trim_trailing_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

RemoteLLM::RemoteLLM(RemoteLLMConfig cfg) : cfg_(std::move(cfg)) {
    cfg_.base_url = trim_trailing_slash(cfg_.base_url);
    name_ = "remote:" + cfg_.model;
    current_ = default_profile_for(ModelTier::Deep);
}

RemoteLLM::~RemoteLLM() = default;

bool RemoteLLM::available() const {
    // 没有端点或模型名就不算可用。api_key 允许为空 —— 本地 vLLM /
    // llama.cpp server 往往不校验密钥，此时 base_url 已足够。
    return !cfg_.base_url.empty() && !cfg_.model.empty();
}

bool RemoteLLM::apply_profile(const LLMGenerationProfile& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = p;
    return true;
}

LLMGenerationProfile RemoteLLM::profile() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

void RemoteLLM::stop() {
    stopping_.store(true);
}

RemoteLLM::Stats RemoteLLM::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void RemoteLLM::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats{};
}

// ========== 请求组装 ==========

std::string RemoteLLM::build_request_json(const ChatRequest& req,
                                          const LLMGenerationProfile& p,
                                          const RemoteLLMConfig& cfg) {
    json j;
    j["model"] = cfg.model;
    j["stream"] = true;

    // 只发该 API 认识的参数。top_k / top_p 不是 OpenAI 标准字段，
    // 硬塞会拿到 400。
    j["temperature"] = p.temperature;
    j["max_tokens"] = p.max_tokens;

    if (cfg.include_usage) {
        j["stream_options"] = json{{"include_usage", true}};
    }

    json msgs = json::array();
    for (const auto& m : req.messages) {
        json mj;
        mj["role"] = m.role;
        mj["content"] = m.content;
        if (!m.name.empty()) mj["name"] = m.name;
        if (!m.tool_call_id.empty()) mj["tool_call_id"] = m.tool_call_id;
        msgs.push_back(std::move(mj));
    }
    j["messages"] = std::move(msgs);

    if (!req.tools.empty()) {
        json tools = json::array();
        for (const auto& t : req.tools) {
            json fn;
            fn["name"] = t.name;
            fn["description"] = t.description;
            if (t.json_schema.empty()) {
                fn["parameters"] = json{{"type", "object"}, {"properties", json::object()}};
            } else {
                fn["parameters"] = json::parse(t.json_schema, nullptr, false);
                if (fn["parameters"].is_discarded()) {
                    fn["parameters"] = json{{"type", "object"}};
                }
            }
            tools.push_back(json{{"type", "function"}, {"function", std::move(fn)}});
        }
        j["tools"] = std::move(tools);
        j["tool_choice"] = "auto";
    }

    return j.dump();
}

// ========== SSE 增量解析 ==========

bool RemoteLLM::parse_delta(const std::string& data_line, std::string& out_text,
                             ToolCallDelta* out_tool, bool* out_done) {
    out_text.clear();
    if (out_done) *out_done = false;

    // [DONE] 是流结束标记
    if (data_line == "[DONE]") {
        if (out_done) *out_done = true;
        return false;
    }

    json j = json::parse(data_line, nullptr, false);
    if (j.is_discarded()) return false;   // 心跳/非 JSON 行，忽略

    // 错误帧：{"error": {...}}
    if (j.contains("error") && j["error"].is_object()) {
        std::string msg = j["error"].value("message", "unknown");
        LOG_WARN("Remote LLM error frame: {}", msg);
        return false;
    }

    auto choices = j.find("choices");
    if (choices == j.end() || !choices->is_array() || choices->empty()) {
        // usage-only 帧
        return false;
    }

    const auto& ch = (*choices)[0];
    if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) {
        std::string fr = ch["finish_reason"].get<std::string>();
        if (!fr.empty() && fr != "stop" && out_done) {
            *out_done = true;
        }
    }

    auto delta = ch.find("delta");
    if (delta == ch.end() || !delta->is_object()) return false;

    // 文本增量
    if (delta->contains("content") && (*delta)["content"].is_string()) {
        out_text = (*delta)["content"].get<std::string>();
    }

    // 工具调用增量
    if (out_tool && delta->contains("tool_calls") && (*delta)["tool_calls"].is_array()) {
        for (const auto& tc : (*delta)["tool_calls"]) {
            if (tc.contains("index") && tc["index"].is_number_integer()) {
                out_tool->id = "call_" + std::to_string(tc["index"].get<int>());
            }
            if (tc.contains("id") && tc["id"].is_string()) {
                out_tool->id = tc["id"].get<std::string>();
            }
            auto fn = tc.find("function");
            if (fn != tc.end() && fn->is_object()) {
                if (fn->contains("name") && (*fn)["name"].is_string()) {
                    out_tool->name = (*fn)["name"].get<std::string>();
                }
                if (fn->contains("arguments") && (*fn)["arguments"].is_string()) {
                    out_tool->arguments += (*fn)["arguments"].get<std::string>();
                }
            }
        }
        if (!out_tool->name.empty()) out_tool->done = true;
        return !out_text.empty() || !out_tool->name.empty();
    }

    return !out_text.empty();
}

// ========== 请求执行 ==========

RemoteLLM::ChatResult RemoteLLM::chat(
    const ChatRequest& req, const std::function<void(const std::string&)>& on_token,
    std::shared_ptr<CancelToken> cancel) {
    ChatResult res;
    const double t0 = now_ms();
    stopping_.store(false);

    if (!available()) {
        res.error = "remote llm not configured";
        return res;
    }

    LLMGenerationProfile p = profile();
    const std::string payload = build_request_json(req, p, cfg_);

    http::Request hreq;
    hreq.url = cfg_.base_url + "/chat/completions";
    hreq.method = "POST";
    hreq.timeout_ms = cfg_.timeout_ms;
    hreq.cancel = cancel;
    hreq.body = payload;
    hreq.headers["Content-Type"] = "application/json";
    hreq.headers["Accept"] = "text/event-stream";
    if (!cfg_.api_key.empty()) {
        hreq.headers["Authorization"] = "Bearer " + cfg_.api_key;
    }

    // 工具调用累积器：按 index 归并增量
    ToolCallDelta tool_acc;
    std::string content;
    bool first_token_seen = false;
    bool stream_done = false;

    http::StreamRequest sreq;
    sreq.base = std::move(hreq);
    sreq.on_headers = [](int status) {
        LOG_DEBUG("Remote LLM response headers: {}", status);
    };
    sreq.on_chunk = [&](const std::string& data) -> bool {
        if (stopping_.load()) return false;
        if (cancel && cancel->cancelled()) return false;

        std::string text;
        bool done = false;
        const bool has = parse_delta(data, text,
                                      req.on_tool_call ? &tool_acc : nullptr, &done);
        if (done && !stream_done) stream_done = true;

        if (has && !text.empty()) {
            if (!first_token_seen) {
                first_token_seen = true;
                res.first_token_ms = now_ms() - t0;
            }
            content += text;
            if (on_token) on_token(text);
        }
        // 返回 true 继续读流；false 提前结束
        return !stopping_.load() && !(cancel && cancel->cancelled());
    };

    const double t1 = now_ms();
    http::StreamResponse sresp = http::stream(sreq);
    const double elapsed = now_ms() - t1;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.requests;
        stats_.total_ms += elapsed;
        if (res.first_token_ms > 0) {
            stats_.ttft_ema_ms = stats_.ttft_ema_ms == 0.0
                                     ? res.first_token_ms
                                     : 0.3 * res.first_token_ms +
                                           0.7 * stats_.ttft_ema_ms;
        }
    }

    if (cancel && cancel->cancelled()) {
        res.cancelled = true;
        res.content = std::move(content);
        return res;
    }
    if (stopping_.load()) {
        res.cancelled = true;
        res.content = std::move(content);
        return res;
    }

    if (!sresp.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.failures;
        res.error = sresp.error;
        // 服务端报错时把 body 带上 —— OpenAI 的错误信息在 body 里
        if (!sresp.body.empty()) {
            json ej = json::parse(sresp.body, nullptr, false);
            if (!ej.is_discarded() && ej.contains("error")) {
                res.error += ": " + ej["error"].value("message", sresp.body.substr(0, 200));
            } else {
                res.error += ": " + sresp.body.substr(0, 200);
            }
        }
        LOG_WARN("Remote LLM request failed: {}", res.error);
        res.content = std::move(content);
        return res;
    }

    res.ok = true;
    res.content = std::move(content);
    if (req.on_tool_call && !tool_acc.name.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.tool_call_rounds;
    }
    LOG_INFO("Remote LLM done: {} chars, ttft={}ms, total={}ms",
             res.content.size(), static_cast<int>(res.first_token_ms),
             static_cast<int>(elapsed));
    return res;
}

void RemoteLLM::generate_stream(const std::string& prompt,
                                const std::function<void(const std::string&)>& on_token,
                                std::shared_ptr<CancelToken> cancel) {
    ChatRequest req;
    RemoteMessage m;
    m.role = "user";
    m.content = prompt;
    req.messages.push_back(std::move(m));
    chat(req, on_token, cancel);
}

}  // namespace voice_agent

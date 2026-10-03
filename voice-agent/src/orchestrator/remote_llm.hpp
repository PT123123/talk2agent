// src/orchestrator/remote_llm.hpp
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/cancel_token.hpp"
#include "orchestrator/model_router.hpp"

namespace voice_agent {

// ========== 远程模型配置 ==========
struct RemoteLLMConfig {
    // OpenAI 兼容端点。留空 base_url 但填了 host/port 时按
    // http://host:port/v1 组装（本地 vLLM / llama.cpp server 场景）。
    std::string base_url{"https://api.openai.com/v1"};
    std::string api_key;
    std::string model{"gpt-4o-mini"};
    int  timeout_ms{30000};
    int  connect_timeout_ms{8000};
    // 是否发 stream_options.include_usage（部分服务端不支持）
    bool include_usage{false};
};

// ========== 一条对话消息 ==========
struct RemoteMessage {
    std::string role;      // system / user / assistant / tool
    std::string content;
    std::string name;      // tool 消息用
    std::string tool_call_id;
};

// ========== RemoteLLM ==========
// 实现 IModelEngine，对接 OpenAI 兼容的 /chat/completions。
//
// 为什么需要它：DEEP 档必须真的"更强"。R0-R6 的分档都只靠采样参数，
// 本质上还是同一个本地模型 —— 架构上叫分档，能力上没有分档。
//
// 设计要点：
//   1. **SSE 逐 token 转发**。on_token 在收到 data 行时被立刻调用，
//      这是语音场景首响应延迟的全部关键。
//   2. **取消要真的生效**。用户打断后必须立即停，否则会对着空气继续说。
//   3. **available() 含配置检查**。没有 api_key 就不算可用，
//      ModelRouter 会据此降级到本地 NORMAL/FAST。
//   4. **失败不抛异常**。网络错误填进统计，由 Router 决定降级。
class RemoteLLM : public IModelEngine {
public:
    explicit RemoteLLM(RemoteLLMConfig cfg);
    ~RemoteLLM() override;

    std::string name() const override { return name_; }
    bool available() const override;
    bool supports_tools() const override { return true; }

    // 远程模型用 temperature / max_tokens；top_k / top_p 不适用于该 API。
    // 不支持的字段会在内部被忽略，而不是发给服务端导致 400。
    bool apply_profile(const LLMGenerationProfile& p) override;
    LLMGenerationProfile profile() const override;

    void generate_stream(const std::string& prompt,
                         const std::function<void(const std::string&)>& on_token,
                         std::shared_ptr<CancelToken> cancel) override;

    void stop() override;

    // ---- 扩展能力 ----

    // 带完整消息历史 + 工具定义的多轮生成（Agent 模式用）。
    // on_token 收到增量文本；工具调用通过 on_tool_call 回传。
    struct ToolSpec {
        std::string name;
        std::string description;
        std::string json_schema;
    };
    struct ToolCallDelta {
        std::string id;
        std::string name;
        std::string arguments;   // 增量拼接
        bool done{false};
    };

    using ToolCallSink = std::function<void(const ToolCallDelta&)>;

    struct ChatRequest {
        std::vector<RemoteMessage> messages;
        std::vector<ToolSpec> tools;
        ToolCallSink on_tool_call;   // 可空
    };

    struct ChatResult {
        bool ok{false};
        std::string error;
        std::string content;
        int  prompt_tokens{0};
        int  completion_tokens{0};
        bool cancelled{false};
        double first_token_ms{0.0};   // 首个 token 到达耗时（TTFT 观测）
    };

    ChatResult chat(const ChatRequest& req,
                    const std::function<void(const std::string&)>& on_token,
                    std::shared_ptr<CancelToken> cancel);

    // ---- 观测 ----
    struct Stats {
        int  requests{0};
        int  failures{0};
        int  tool_call_rounds{0};
        double ttft_ema_ms{0.0};
        double total_ms{0.0};
    };
    Stats stats() const;
    void reset_stats();

    const RemoteLLMConfig& config() const { return cfg_; }

    // ---- 纯函数（公开以便离线单测）----
    // 组装请求 JSON。刻意不把 top_k/top_p 塞进去：OpenAI 兼容 API 不认，
    // 硬塞会拿到 400。
    static std::string build_request_json(const ChatRequest& req,
                                          const LLMGenerationProfile& p,
                                          const RemoteLLMConfig& cfg);
    // 解析一条 SSE data 行。返回 true 表示该行含增量。
    static bool parse_delta(const std::string& data_line, std::string& out_text,
                            ToolCallDelta* out_tool, bool* out_done);

private:
    RemoteLLMConfig cfg_;
    std::string name_;
    mutable std::mutex mutex_;
    LLMGenerationProfile current_;
    std::atomic<bool> stopping_{false};
    Stats stats_;
};

}  // namespace voice_agent

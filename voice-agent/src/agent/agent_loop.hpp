// src/agent/agent_loop.hpp
#pragma once
#include "core/types.hpp"
#include "core/cancel_token.hpp"
#include "agent/tool_registry.hpp"
#include "llm/llm.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace voice_agent {

// ========== 单轮 Agent 结果 ==========
struct AgentTurnResult {
    std::string final_text;   // 最终回答纯文本
    int tool_rounds{0};       // 实际执行的工具轮次
    bool timed_out{false};
    bool cancelled{false};
};

// ========== Agent 循环：生成 → 解析工具 → 执行 → 回填 → 再生成 ==========
// 轮次上限 max_rounds；工具必须响应取消令牌。
class AgentLoop {
public:
    // 文本 token 流式回调（可接到 TTS 实时合成）
    using TokenSink = std::function<void(const std::string&)>;

    AgentLoop(LLM& llm, ToolRegistry& registry, ToolExecutor& executor);

    void set_system_prompt(std::string p);
    void set_max_rounds(int n) { max_rounds_ = n; }
    void set_tool_timeout_ms(int ms) { tool_timeout_ms_ = ms; }
    void set_token_sink(TokenSink sink) { token_sink_ = std::move(sink); }

    // 运行一轮完整对话。user_text 为用户输入；返回最终回答。
    AgentTurnResult run(const std::string& user_text, std::shared_ptr<CancelToken> cancel);

    // 历史访问（调试用）
    const std::vector<Message>& history() const { return history_; }

private:
    std::string build_prompt_() const;
    void append_history_(Message msg);

    LLM&      llm_;
    ToolRegistry& registry_;
    ToolExecutor& executor_;

    std::vector<Message> history_;
    std::string system_prompt_;
    int max_rounds_{3};
    int tool_timeout_ms_{5000};
    TokenSink token_sink_;
};

}  // namespace voice_agent
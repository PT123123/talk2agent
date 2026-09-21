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
    int tool_calls{0};        // 实际执行的工具调用总数
    bool timed_out{false};
    bool cancelled{false};
};

// ========== 单轮处理计时 ==========
// 阶段名（大写）与耗时（毫秒）。供 GUI 绘制“轮次时间轴”与延时面板：
//   llm_first   —— 从本轮开始到首个文本 token 的耗时至
//   llm_generate—— 各轮 LLM 生成耗时的累计
//   tools       —— 工具并行执行的累计耗时
using TraceFn = std::function<void(const std::string& stage, double ms)>;

// 工具调用事件（供 GUI 区分“回复”与“工具调用”）：line 为人类可读描述
using ToolReportFn = std::function<void(const std::string& line)>;

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
    // 本轮允许使用的工具名集合；为空表示不暴露任何工具（模型无从调用）。
    // 用于"工具意图门控"：只有用户明确要求（搜索/记忆/时间）时才放行对应工具。
    void set_enabled_tools(std::vector<std::string> tools) {
        enabled_tools_ = std::move(tools);
    }
    void set_token_sink(TokenSink sink) { token_sink_ = std::move(sink); }
    void set_trace(TraceFn fn) { trace_ = std::move(fn); }
    void set_tool_report(ToolReportFn fn) { tool_report_ = std::move(fn); }

    // 运行一轮完整对话。user_text 为用户输入；返回最终回答。
    AgentTurnResult run(const std::string& user_text, std::shared_ptr<CancelToken> cancel);

    // 历史访问（调试用）
    const std::vector<Message>& history() const { return history_; }

private:
    std::string build_prompt_() const;
    void append_history_(Message msg);
    void emit_stage_(const std::string& stage, double ms) const;

    LLM&      llm_;
    ToolRegistry& registry_;
    ToolExecutor& executor_;

    std::vector<Message> history_;
    std::string system_prompt_;
    std::vector<std::string> enabled_tools_;   // 空 = 本轮不暴露任何工具
    int max_rounds_{3};
    int tool_timeout_ms_{5000};
    TokenSink token_sink_;
    TraceFn trace_;
    ToolReportFn tool_report_;
};

}  // namespace voice_agent
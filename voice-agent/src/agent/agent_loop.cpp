// src/agent/agent_loop.cpp
#include "agent_loop.hpp"
#include "util/log.hpp"
#include "grammar.hpp"
#include <algorithm>
#include <sstream>

namespace voice_agent {

namespace {
// 从助手输出中剥离 <tool_call> 块，仅保留最终可播报文本
std::string strip_tool_blocks(const std::string& text) {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t open = text.find(kToolCallOpen, pos);
        if (open == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        out.append(text, pos, open - pos);
        size_t close = text.find(kToolCallClose, open + std::string(kToolCallOpen).size());
        if (close == std::string::npos) break;
        pos = close + std::string(kToolCallClose).size();
    }
    return out;
}
}  // namespace

AgentLoop::AgentLoop(LLM& llm, ToolRegistry& registry, ToolExecutor& executor)
    : llm_(llm), registry_(registry), executor_(executor) {}

void AgentLoop::set_system_prompt(std::string p) {
    system_prompt_ = std::move(p);
}

void AgentLoop::append_history_(Message msg) {
    history_.push_back(std::move(msg));
}

std::string AgentLoop::build_prompt_() const {
    std::ostringstream o;
    if (!system_prompt_.empty()) {
        o << "[system]\n" << system_prompt_ << "\n\n";
    }
    // 工具清单
    auto defs = registry_.tool_defs();
    if (!defs.empty()) {
        o << "[available tools]\n";
        for (auto& d : defs) {
            o << "- " << d.name << ": " << d.description << "\n";
        }
        o << "如需调用工具，在文本中使用标记块，例如：\n"
          << kToolCallOpen
          << "{\"name\":\"web_search\",\"arguments\":{\"query\":\"...\"}}"
          << kToolCallClose << "\n";
        o << "\n";
    }
    for (auto& m : history_) {
        o << "[" << m.role << "] " << (m.name.empty() ? "" : m.name + ": ") << m.content << "\n";
    }
    return o.str();
}

AgentTurnResult AgentLoop::run(const std::string& user_text, std::shared_ptr<CancelToken> cancel) {
    AgentTurnResult result;
    if (cancel && cancel->cancelled()) {
        result.cancelled = true;
        return result;
    }

    append_history_(Message{"user", user_text});

    for (int round = 0; round < max_rounds_; ++round) {
        if (cancel && cancel->cancelled()) {
            result.cancelled = true;
            break;
        }

        // 1. 生成
        std::string prompt = build_prompt_();
        std::string raw;
        std::string staged;  // 当前文本 token

        llm_.set_cancel_token(cancel);
        llm_.generate_stream(prompt, [&](const LLMResponse& chunk) {
            if (chunk.text.empty()) return;
            raw += chunk.text;

            // 已进入工具块的内容不流式播报
            size_t open = raw.rfind(kToolCallOpen);
            size_t close_at = staged.find(kToolCallClose);
            bool in_tool = (open != std::string::npos) &&
                           (close_at == std::string::npos || open > close_at);
            if (!in_tool) {
                staged += chunk.text;
                if (token_sink_) {
                    // 只把非工具块文本交给 sink
                    std::string plain = strip_tool_blocks(chunk.text);
                    if (!plain.empty()) token_sink_(plain);
                }
            }
        });

        if (cancel && cancel->cancelled()) {
            result.cancelled = true;
            append_history_(Message{"assistant", strip_tool_blocks(raw)});
            break;
        }

        // 2. 解析工具调用
        auto calls = parse_tool_calls(raw);

        // 记住助手说了什么（不含工具块）
        std::string assistant_plain = strip_tool_blocks(raw);
        append_history_(Message{"assistant", assistant_plain});

        if (calls.empty()) {
            // 没有工具调用 → 这就是最终回答
            result.final_text = assistant_plain;
            break;
        }

        // 3. 记录是什么工具，然后执行
        result.tool_rounds++;
        LOG_INFO("Agent round {}: executing {} tool call(s)", result.tool_rounds, calls.size());

        auto tool_results = executor_.execute_parallel(calls, cancel, tool_timeout_ms_);

        for (auto& r : tool_results) {
            std::string line = r.is_error ? ("tool error: " + r.content) : r.content;
            append_history_(Message{"tool", line});
            LOG_INFO("  tool result ({}): {}", r.call_id, line.substr(0, 120));
        }

        // 最后一轮仍无简洁回答时，防止死循环
        if (result.tool_rounds >= max_rounds_) {
            result.final_text = assistant_plain;
            result.timed_out = true;
            break;
        }
    }

    if (result.final_text.empty() && !result.cancelled) {
        result.final_text = "抱歉，我暂时无法给出有效回答。";
    }
    return result;
}

}  // namespace voice_agent
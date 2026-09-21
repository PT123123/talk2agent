// src/agent/agent_loop.cpp
#include "agent_loop.hpp"
#include "util/log.hpp"
#include "grammar.hpp"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace voice_agent {

namespace {
inline double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// 从完整助手输出中剥离 <tool_call> 块，仅保留最终可播报文本
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

// ========== 流式输出清洗 ==========
// LLM 逐 token 吐出的字节会在任意位置切分 UTF-8 多字节字符，且 <tool_call> 块
// 也可能被切成两段。此状态机把"切乱的字节流"收敛为"干净的流式文本"：
//   - 不完整的 UTF-8 尾字节跨 chunk 缓存，拼完整后再解码，杜绝乱码 ''
//   - 工具块跨 chunk 识别并整块剥离，不使其漏进对话/播报流
class AssistantStream {
public:
    using Sink = std::function<void(const std::string&)>;
    explicit AssistantStream(Sink sink) : sink_(std::move(sink)) {}

    void push(const std::string& chunk) {
        work_ += chunk;
        process_();
    }
    // 生成结束：把残留的未完成字节一并吐出（正常情况下应为空或字符尾部）
    void flush() {
        if (!in_tool_ && !work_.empty()) {
            if (sink_) sink_(work_);
            work_.clear();
        }
    }

private:
    // 放行 work_[0, complete_len)，其中该区间是已确认完整的一段 UTF-8 文本
    void emit_normal_(size_t complete_len) {
        if (complete_len == 0) return;
        if (sink_) sink_(work_.substr(0, complete_len));
        work_.erase(0, complete_len);
    }

    static size_t utf8_complete_prefix(const std::string& s, size_t limit) {
        size_t i = limit;
        unsigned char c = 0;
        size_t need = 0;
        // 从尾部向前找最后一个"前导字节"，判断它开启的字符是否完整落在 limit 内
        int steps = 0;
        for (; i > 0; --i) {
            c = (unsigned char)s[i - 1];
            if ((c & 0xC0) == 0x80) { ++steps; continue; }  // 连续字节
            if ((c & 0x80) == 0) return limit;              // 遇到 ASCII：此前全部完整
            if ((c & 0xE0) == 0xC0) need = 2;
            else if ((c & 0xF0) == 0xE0) need = 3;
            else if ((c & 0xF8) == 0xF0) need = 4;
            else need = 1;                                  // 非法前导字节，按单字节处置
            // 该前导字节位于 i-1，需长度 need；若越出 limit 则把整段多字节字符留待拼接
            break;
        }
        // 全是连续字节（无前导字节）→ 视为不完整，全部缓存在 work_
        if (i == 0) return 0;
        if (s.size() - (i - 1) >= need) return limit;       // 完整字符
        return i - 1;                                       // 保留从 i-1 起的未完成字符
    }

    void process_() {
        if (in_tool_) { consume_tool_(); return; }
        while (!work_.empty()) {
            size_t open = work_.find(kToolCallOpen);
            if (open == std::string::npos) {
                // 无工具块：只放行完整字节
                size_t done = utf8_complete_prefix(work_, work_.size());
                emit_normal_(done);
                return;  // 残留（若有）是未完成字符，留在 work_ 中
            }
            // 有工具块：先放行其前文本（不能跨块切）。limit=open，保证不吞掉工具前字符
            size_t keep = utf8_complete_prefix(work_, open);
            emit_normal_(keep);
            open = work_.find(kToolCallOpen);
            if (open == std::string::npos) return;
            work_.erase(0, open + std::string(kToolCallOpen).size());
            in_tool_ = true;
            consume_tool_();
            return;
        }
    }

    void consume_tool_() {
        size_t close = work_.find(kToolCallClose);
        if (close != std::string::npos) {
            work_.erase(0, close + std::string(kToolCallClose).size());
            in_tool_ = false;
            process_();  // 工具结束后继续处理其后文本
            return;
        }
        // 只保留足以识别"被切开的结束标记"的尾部窗口，避免无限增长
        constexpr size_t kKeep = 64;
        if (work_.size() > kKeep) work_.erase(0, work_.size() - kKeep);
    }

    std::string work_;
    bool in_tool_{false};
    Sink sink_;
};
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
    // 工具清单（仅列出本轮被意图门控放行的工具；无放行则完全不暴露，
    // 模型不会生成工具块，杜绝误触发 web_search / memory_*）
    auto defs = registry_.tool_defs();
    if (!defs.empty()) {
        std::vector<ToolDef> allowed;
        for (auto& d : defs) {
            if (std::find(enabled_tools_.begin(), enabled_tools_.end(), d.name) !=
                enabled_tools_.end()) {
                allowed.push_back(d);
            }
        }
        if (!allowed.empty()) {
            o << "[available tools]\n";
            for (auto& d : allowed) {
                o << "- " << d.name << ": " << d.description << "\n";
            }
            o << "仅在用户明确要求时才调用工具，且必须使用标记块，例如：\n"
              << kToolCallOpen
              << "{\"name\":\"web_search\",\"arguments\":{\"query\":\"...\"}}"
              << kToolCallClose << "\n";
            o << "\n";
        }
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

    const double t_start = now_ms();
    double first_token_ms = 0.0;
    bool first_seen = false;
    double generate_total = 0.0;
    double tools_total = 0.0;

    append_history_(Message{"user", user_text});

    for (int round = 0; round < max_rounds_; ++round) {
        if (cancel && cancel->cancelled()) {
            result.cancelled = true;
            break;
        }

        // 1. 生成
        std::string prompt = build_prompt_();
        std::string raw;
        const double t_gen0 = now_ms();

        // 流式输出经状态机清洗：跨 chunk 拼完整 UTF-8、剥离工具块后递给 sink
        AssistantStream stream(token_sink_);
        llm_.set_cancel_token(cancel);
        llm_.generate_stream(prompt, [&](const LLMResponse& chunk) {
            if (chunk.text.empty()) return;
            if (!first_seen) {
                first_seen = true;
                first_token_ms = now_ms() - t_start;
            }
            raw += chunk.text;
            stream.push(chunk.text);
        });
        stream.flush();
        generate_total += now_ms() - t_gen0;

        if (cancel && cancel->cancelled()) {
            result.cancelled = true;
            append_history_(Message{"assistant", strip_tool_blocks(raw)});
            break;
        }

        // 2. 解析工具调用
        auto calls = parse_tool_calls(raw);

        // 双保险：即使模型生成了未放行工具的工具块（如乱调用 web_search），
        // 也直接丢弃，不执行、不把结果回填进上下文。
        {
            std::vector<ToolCall> allowed;
            for (auto& c : calls) {
                if (std::find(enabled_tools_.begin(), enabled_tools_.end(), c.name) !=
                    enabled_tools_.end()) {
                    allowed.push_back(std::move(c));
                } else {
                    LOG_INFO("Tool '{}' not enabled this turn - dropped", c.name);
                }
            }
            calls.swap(allowed);
        }

        // 记住助手说了什么（不含工具块）
        std::string assistant_plain = strip_tool_blocks(raw);
        append_history_(Message{"assistant", assistant_plain});

        if (calls.empty()) {
            // 没有工具调用 → 这就是最终回答
            result.final_text = assistant_plain;
            break;
        }

        // 3. 记录是什么工具并上报事件，然后执行
        result.tool_rounds++;
        result.tool_calls += static_cast<int>(calls.size());
        LOG_INFO("Agent round {}: executing {} tool call(s)", result.tool_rounds, calls.size());

        if (tool_report_) {
            for (const auto& c : calls) {
                tool_report_("[" + c.name + "] " + c.arguments_json.substr(0, 200));
            }
        }

        const double t_tools0 = now_ms();
        auto tool_results = executor_.execute_parallel(calls, cancel, tool_timeout_ms_);
        tools_total += now_ms() - t_tools0;

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

    if (first_seen) emit_stage_("llm_first", first_token_ms);
    if (generate_total > 0.0) emit_stage_("llm_generate", generate_total);
    if (tools_total > 0.0) emit_stage_("tools", tools_total);

    if (result.final_text.empty() && !result.cancelled) {
        result.final_text = "抱歉，我暂时无法给出有效回答。";
    }
    return result;
}

void AgentLoop::emit_stage_(const std::string& stage, double ms) const {
    if (trace_) trace_(stage, ms);
}

}  // namespace voice_agent
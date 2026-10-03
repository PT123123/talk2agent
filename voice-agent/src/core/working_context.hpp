// src/core/working_context.hpp
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace voice_agent {

// ========== Working Context（工作上下文）============
// 与 Long-Term Memory 的根本区别：
//   Working Context = 本次会话的"临时草稿纸"，随话题演进而变，随会话结束而丢弃。
//   Long-Term Memory = 跨会话仍然成立的事实。
//
// 例：用户说"我现在正在做 SlideTrace" —— 这不该进长期记忆（他可能只是这阵子在做），
//     而应成为 working_context.active_project = "SlideTrace"。
//     之后用户说"那个时间轴的问题……"，Agent 从 Working Context 解析出"那个"。
class WorkingContext {
public:
    // 设置话题（换话题时调用；旧话题的临时项会过期）
    void set_topic(std::string topic);

    // 通用键值项。ttl_sec = 0 表示跟随话题（换话题即失效）。
    // ttl_sec > 0 表示绝对存活时长（秒）。
    void set(const std::string& key, std::string value, int ttl_sec = 0);

    std::optional<std::string> get(const std::string& key) const;

    bool has(const std::string& key) const;
    void unset(const std::string& key);
    void clear();

    const std::string& topic() const { return topic_; }

    // 最近提到的实体（按时间倒序），用于"那个/刚才那个"的指代消解
    void push_entity(std::string entity);
    const std::vector<std::string>& recent_entities() const { return entities_; }
    std::optional<std::string> latest_entity() const;

    // 当前活跃项目（专门留一个槽位，指代消解命中率最高）
    void set_active_project(std::string p);
    std::optional<std::string> active_project() const { return get("active_project"); }

    // 换话题：清掉所有 ttl_sec==0 的临时项（实体与项目名也清）
    void on_topic_changed(std::string new_topic);

    // 过期清理（定时调用）。返回被清掉的项数。
    size_t expire();

    // 快照（供 trace / replay 记录）。返回的 map 自身已按过期过滤。
    std::map<std::string, std::string> snapshot() const;
    // 一次性拿到快照 + 最近实体，避免调用方为拿两样东西锁两次
    struct Snapshot {
        std::map<std::string, std::string> items;
        std::string latest_entity;
    };
    Snapshot snapshot_with_entity() const;

private:
    struct Item {
        std::string value;
        int64_t expire_at_ms{0};   // 0 = 不过期
    };

    mutable std::mutex mutex_;
    std::string topic_;
    std::map<std::string, Item> items_;
    std::vector<std::string> entities_;

    static int64_t now_ms();
};

// ========== Context 分层 ==========
// 拼装 prompt 时的优先级分层，从高到低。目的是避免"last N messages"
// 在长对话里无限膨胀，同时保证当前话题的上下文永远不被挤出。
enum class ContextLayer {
    P0_CurrentInput,     // 当前这轮用户输入
    P1_CurrentTopic,     // 当前正在讨论的话题
    P2_TaskState,        // 当前任务/后台任务状态
    P3_RecentTurns,      // 最近几轮对话
    P4_WorkingContext,   // 工作上下文
    P5_RelevantMemory,   // 召回的长期记忆
    P6_ToolResults,      // 工具结果
    P7_OlderHistory      // 更旧历史（摘要化）
};

inline const char* context_layer_to_string(ContextLayer l) {
    switch (l) {
        case ContextLayer::P0_CurrentInput:   return "P0_CurrentInput";
        case ContextLayer::P1_CurrentTopic:   return "P1_CurrentTopic";
        case ContextLayer::P2_TaskState:      return "P2_TaskState";
        case ContextLayer::P3_RecentTurns:    return "P3_RecentTurns";
        case ContextLayer::P4_WorkingContext: return "P4_WorkingContext";
        case ContextLayer::P5_RelevantMemory: return "P5_RelevantMemory";
        case ContextLayer::P6_ToolResults:    return "P6_ToolResults";
        case ContextLayer::P7_OlderHistory:   return "P7_OlderHistory";
    }
    return "Unknown";
}

// ========== 上下文条目（P3/P6 等层用）============
// 带来源标签：多来源并存而非互相覆盖。
// 原先 tool_results_ 只有一个 vector，后写进来的（后台缓存注入）会把
// 先写进去的本轮真实工具结果顶掉 —— 那正好是最不该丢的信息。
struct ContextEntry {
    std::string source;     // "tool" / "background" / "trace" ...
    std::string content;
};

// 一条对话轮次
struct ConversationTurn {
    std::string user;
    std::string assistant;
    int64_t ts_ms{0};
};

// ========== ContextManager ==========
// 按 P0~P7 动态拼装上下文，各层有独立 token 预算。
// 低优先级层在预算不足时先被丢弃 —— 这就是长对话不膨胀的关键。
class ContextManager {
public:
    struct Config {
        int recent_turns{6};         // P3 保留最近几轮
        int working_context_max{400};// P4 字符预算
        int memory_max{600};         // P5 字符预算
        int tool_results_max{600};   // P6 字符预算
        int total_char_budget{4000}; // 总预算
        int max_session_turns{200};  // 会话内保留的轮次上限
    };

    explicit ContextManager(Config cfg = {}) : cfg_(cfg) {}

    // 记录一轮对话
    void add_turn(std::string user, std::string assistant);

    // 注入召回的记忆（本轮相关）
    void set_relevant_memory(std::vector<std::string> memories);

    // ---- P6 层：多来源并存，不互相覆盖 ----
    // 本轮真实工具结果（source="tool"）
    void add_tool_result(std::string content);
    // 后台任务结果（source="background"）。与上面的工具结果并存。
    void add_background_result(std::string content);
    // 清空 P6（每轮开始时调用）
    void clear_results();

    // 注入任务状态摘要（后台任务在跑什么）
    void set_task_state_summary(std::string s);

    // 组装最终上下文文本（不含 system prompt，不含当前这轮输入）
    std::string build_context() const;

    // 当前话题（代理到 WorkingContext）
    const std::string& topic() const { return working_.topic(); }
    void set_topic(std::string t) { working_.set_topic(std::move(t)); }
    void on_topic_changed(std::string t) { working_.on_topic_changed(std::move(t)); }
    WorkingContext& working() { return working_; }
    const WorkingContext& working() const { return working_; }

    // 本会话的轮次数 / 是否已触发老历史摘要
    size_t turn_count() const;
    bool has_older_history() const;

    const std::vector<ConversationTurn>& turns() const { return turns_; }

    void clear();

private:
    void summarize_old_();

    Config cfg_;
    mutable std::mutex mutex_;
    std::vector<ConversationTurn> turns_;
    std::vector<std::string> memory_;
    std::vector<ContextEntry> results_;      // P6：多来源并存
    std::string task_state_summary_;
    WorkingContext working_;

    // ---- P7 老历史滚动摘要 ----
    // 超出 recent_turns 窗口的轮次会被压缩成摘要，而不是简单丢弃 ——
    // 简单丢弃会让模型"忘记"前面聊过什么，长对话里表现为反复问同一件事。
    std::string older_summary_;
    size_t summarized_count_{0};      // 已被摘要覆盖的轮次总数
    size_t summarize_every_{8};       // 每积累多少轮触发一次摘要
    size_t summary_max_chars_{600};

public:
    // 设置摘要策略（每 N 轮摘要一次，摘要长度上限）
    void set_summary_policy(size_t every_n, size_t max_chars);

    // 老历史摘要（可能为空）
    std::string older_summary() const;

    // 已被摘要覆盖的轮次数
    size_t summarized_count() const;
};

}  // namespace voice_agent

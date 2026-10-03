// src/orchestrator/response_policy.hpp
#pragma once
#include <atomic>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "orchestrator/user_intent.hpp"

namespace voice_agent {

// ========== ResponsePolicy 的动作 ==========
// 关键：LLM 不应该每次都直接决定完整答案。
// 先由这一层（零模型开销的规则）决定"要不要说、说多深、要不要后台干活"，
// 只有在它判定需要实质内容时，才让 LLM 干活。
enum class ResponseAction {
    Silence,          // 保持安静（合法且经常是正确的选择）
    Backchannel,      // 只回一个附和/接手音（"嗯""我看一下"）
    QuickReply,       // 快速直答，不调大模型
    Answer,           // 正常回答（本地模型即可）
    Search,           // 需要实时事实：先 ack，再后台搜
    Agent,            // 需要操作本地：文件/命令/项目
    DeepReasoning     // 复杂推理：升级到强模型
};

inline const char* response_action_to_string(ResponseAction a) {
    switch (a) {
        case ResponseAction::Silence:        return "silence";
        case ResponseAction::Backchannel:    return "backchannel";
        case ResponseAction::QuickReply:     return "quick_reply";
        case ResponseAction::Answer:         return "answer";
        case ResponseAction::Search:         return "search";
        case ResponseAction::Agent:          return "agent";
        case ResponseAction::DeepReasoning:  return "deep_reasoning";
    }
    return "unknown";
}

// ========== 推理深度 ==========
enum class ResponseDepth {
    Low,      // 一两句
    Medium,   // 常规
    High      // 需要展开
};

inline const char* response_depth_to_string(ResponseDepth d) {
    switch (d) {
        case ResponseDepth::Low:    return "low";
        case ResponseDepth::Medium: return "medium";
        case ResponseDepth::High:   return "high";
    }
    return "unknown";
}

// ========== 模型档位 ==========
enum class ModelTier {
    Fast,        // 小模型/规则：ack、简单闲聊
    Normal,      // 本地中型模型
    Deep,        // 强在线模型
    Agent,       // Local Agent
    Search,      // SearchProvider + LLM
    Background   // 后台专用（结果入 cache 不播报）
};

inline const char* model_tier_to_string(ModelTier t) {
    switch (t) {
        case ModelTier::Fast:       return "FAST";
        case ModelTier::Normal:     return "NORMAL";
        case ModelTier::Deep:       return "DEEP";
        case ModelTier::Agent:      return "AGENT";
        case ModelTier::Search:     return "SEARCH";
        case ModelTier::Background: return "BACKGROUND";
    }
    return "UNKNOWN";
}

// ========== 输入 ==========
struct ResponsePolicyInput {
    std::string user_text;         // ASR 最终文本
    std::string partial_text;      // 当前 partial（用户可能还在说）
    UserSpeechIntent intent{UserSpeechIntent::Content};
    std::string working_context;   // WorkingContext 摘要（可空）
    bool agent_is_speaking{false};
    double measured_llm_ttft_ms{0.0};  // 上一次实测的首 token 延迟，用于自适应预算
    bool tools_available{true};
};

// ========== 输出 ==========
struct ResponseDecision {
    ResponseAction action{ResponseAction::Answer};
    ResponseDepth depth{ResponseDepth::Medium};
    ModelTier tier{ModelTier::Normal};

    bool needs_memory{false};
    bool needs_search{false};
    bool needs_agent{false};

    // 是否允许把耗时工作放到后台（前台先给个短回应）
    bool allow_background{false};

    // 前台该等多久（毫秒）。超时后应转为 Backchannel 或 Silence。
    int latency_budget_ms{1500};

    // 若 action == Backchannel，这里是候选话术（空串 = 保持沉默）
    std::string ack_text;

    // 人类可读的判定理由，写进 trace 便于调参
    std::string reason;

    bool should_speak() const {
        return action != ResponseAction::Silence && !ack_text.empty();
    }
};

// ========== ResponsePolicy ==========
// 纯规则实现。刻意不调大模型 ——
// 判断"我要不要先说一句"用最强模型是浪费 token 且引入不必要延迟。
class ResponsePolicy {
public:
    struct Config {
        int default_latency_budget_ms{1500};
        int search_latency_budget_ms{2500};
        // 实测 TTFT 高于该值时，认为"先说一句"更有必要
        double high_ttft_threshold_ms{900.0};
        // 用户处于 SPEAKING 时，非打断类输入一律先 Silence（不抢话）
        bool never_talk_over_agent{true};
    };

    explicit ResponsePolicy(Config cfg = {}) : cfg_(cfg) {}

    ResponseDecision decide(const ResponsePolicyInput& in) const;

    // 强制静默窗口：用户刚说完话后的这段时间内，Backchannel 也压掉，
    // 避免"我一停你就嗯嗯嗯"。由 Orchestrator 按 TurnDetector 结果设置。
    void set_silence_window_ms(int ms) { silence_window_ms_ = ms; }
    void clear_silence_window() { silence_window_ms_ = 0; }

private:
    Config cfg_;
    std::atomic<int> silence_window_ms_{0};
};

// ========== ProgressUtterancePolicy ==========
// 长任务要不要说一句、说什么。
// 绝不能固定成"让我查一下" —— 每次都一样就是机器味的主要来源。
enum class ProgressKind {
    Silent,       // 不说话
    Acknowledge,  // 接手（"我看一下"）
    Progress      // 报进度（"已经找到三个来源了"）
};

struct ProgressUtteranceInput {
    std::string task_type;        // "search" / "memory" / "agent" / "llm"
    int expected_latency_ms{0};
    int elapsed_ms{0};
    std::string recent_speech;   // 用户最近说了什么（避免复读）
    bool user_seems_impatient{false};
    int turns_spoken_in_task{0}; // 本任务已经说过几句（防话痨）
};

struct ProgressUtterance {
    ProgressKind kind{ProgressKind::Silent};
    std::string text;
};

class ProgressUtterancePolicy {
public:
    ProgressUtterance decide(const ProgressUtteranceInput& in) const;

private:
    // 每个 task_type 的多候选话术，按语境轮换
    static const std::vector<std::string>& ack_pool(const std::string& task_type);
};

}  // namespace voice_agent

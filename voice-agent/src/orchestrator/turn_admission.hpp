// src/orchestrator/turn_admission.hpp
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "core/working_context.hpp"
#include "orchestrator/response_policy.hpp"
#include "orchestrator/user_intent.hpp"

namespace voice_agent {

// ========== 轮次准入配置 ==========
struct TurnAdmissionConfig {
    bool enable_response_policy{true};
    bool enable_topic_supersede{true};
    bool agent_speaking{false};
};

// ========== 一次准入决策的结论 ==========
struct TurnAdmissionResult {
    // 分类结果
    UserSpeechIntent intent{UserSpeechIntent::Content};

    // Policy 决策（intent 为 Backchannel/Interruption 时也有值，便于观测）
    ResponseDecision decision;

    // 是否应该起一轮新的 LLM/TTS 流程
    bool should_start_turn{false};

    // 是否应该打断当前播报（让出话轮）
    bool should_interrupt{false};

    // 是否发生了真正的换话题（连着说两次"算了"不算）
    bool topic_changed{false};

    // 是否需要上层 supersede 同 topic 的在途后台任务
    bool supersede_stale_tasks{false};

    // 人类可读理由（写进 trace）
    std::string reason;
};

// ========== TurnAdmission ==========
// 「用户说了这句话，要不要起一轮」的决策器。
//
// 单独抽出来的原因：这是自然度的总闸门，但真实语音链路要模型 + 音频设备
// 才能跑 —— 把它独立出来，才能脱离模型对"嗯/对/好不打断"、"换话题清上下文"
// 这类行为做精确断言。
//
// 无状态依赖（除 WorkingContext / Policy 的配置），可被 Orchestrator 复用。
class TurnAdmission {
public:
    // external_working != nullptr 时，WorkingContext 由调用方持有并传入 ——
    // Orchestrator 里 ContextManager 已经持有一份，两处各持一份会导致
    // "准入时清了一份、拼 prompt 时读的是另一份"，指代消解永远对不上。
    // 传 nullptr 则内部自持一份（测试与独立使用场景）。
    explicit TurnAdmission(TurnAdmissionConfig cfg = {},
                           WorkingContext* external_working_ptr = nullptr)
        : cfg_(cfg), external_working_(external_working_ptr) {}

    WorkingContext& work() {
        return external_working_ ? *external_working_ : owned_working_;
    }
    const WorkingContext& work() const {
        return external_working_ ? *external_working_ : owned_working_;
    }

    // 主入口：吃一段用户文本，吐出决策。
    TurnAdmissionResult evaluate(const std::string& text);

    // ---- 配置 ----
    void set_agent_speaking(bool v) { cfg_.agent_speaking = v; }
    bool agent_speaking() const { return cfg_.agent_speaking; }
    void set_response_policy_enabled(bool v) { cfg_.enable_response_policy = v; }
    void set_topic_supersede_enabled(bool v) { cfg_.enable_topic_supersede = v; }
    void set_silence_window_ms(int ms) { policy_.set_silence_window_ms(ms); }

    // ---- 观测 ----
    ResponsePolicy& policy() { return policy_; }
    const ResponsePolicy& policy() const { return policy_; }
    UserIntentClassifier& classifier() { return classifier_; }
    WorkingContext& working() { return work(); }
    const WorkingContext& working() const { return work(); }

    UserSpeechIntent last_intent() const { return last_intent_.load(); }

    // 当前轮次 id：只有真正起了轮次才会递增。静默/附和消耗不掉。
    uint64_t current_turn_id() const { return current_turn_id_.load(); }

    void reset();

private:
    TurnAdmissionConfig cfg_;
    UserIntentClassifier classifier_;
    ResponsePolicy policy_;

    // WorkingContext 要么外部持有（Orchestrator），要么内部自持（独立使用）
    WorkingContext owned_working_;
    WorkingContext* external_working_{nullptr};

    std::atomic<UserSpeechIntent> last_intent_{UserSpeechIntent::Content};
    std::atomic<UserSpeechIntent> prev_intent_{UserSpeechIntent::Content};
    std::atomic<uint64_t> current_turn_id_{0};
    std::atomic<uint64_t> next_turn_id_{1};
};

}  // namespace voice_agent

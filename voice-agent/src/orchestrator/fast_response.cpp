// src/orchestrator/fast_response.cpp
#include "fast_response.hpp"

#include <algorithm>
#include <cmath>

namespace voice_agent {

// ========== 延迟自适应 ==========

void FastResponseLayer::record_latency(const std::string& task_type, int ms) {
    if (ms <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = latency_ema_.find(task_type);
    if (it == latency_ema_.end()) {
        latency_ema_[task_type] = static_cast<double>(ms);
    } else {
        it->second = cfg_.ewma_alpha * ms + (1.0 - cfg_.ewma_alpha) * it->second;
    }
}

int FastResponseLayer::estimate_latency_ms(const std::string& task_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = latency_ema_.find(task_type);
    if (it == latency_ema_.end()) return cfg_.default_estimate_ms;
    return static_cast<int>(std::lround(it->second));
}

// ========== 抢答决策 ==========

FastResponsePlan FastResponseLayer::plan(const ResponseDecision& decision,
                                         const std::string& task_type,
                                         const std::string& user_text) const {
    FastResponsePlan p;

    // ---- 1. 开关 ----
    if (!cfg_.enabled) {
        p.reason = "fast response disabled";
        return p;
    }

    // ---- 2. Policy 已经判静默的，别硬抢话 ----
    // 附和、用户还在说、Agent 正在播 —— 任何一种情况下插一句都是打扰。
    if (decision.action == ResponseAction::Silence) {
        p.reason = "policy chose silence: " + decision.reason;
        return p;
    }

    // ---- 3. 话痨闸门 ----
    if (acks_in_turn_.load() >= cfg_.max_acks_per_task) {
        p.reason = "already spoke this turn";
        return p;
    }

    // ---- 4. 预估耗时 ----
    p.expected_ms = estimate_latency_ms(task_type);

    // 够快就别废话 —— 用户宁愿等 300ms 也不要听一句"我看一下"
    if (p.expected_ms < cfg_.ack_threshold_ms) {
        p.decision = AckDecision::Silent;
        p.reason = "fast enough (" + std::to_string(p.expected_ms) + "ms)";
        return p;
    }

    // ---- 5. 已经决定要先说话了：挑一句 ----
    ProgressUtteranceInput in;
    in.task_type = task_type;
    in.expected_latency_ms = p.expected_ms;
    in.recent_speech = user_text;
    in.turns_spoken_in_task = acks_in_turn_.load();

    ProgressUtterancePolicy pol;
    ProgressUtterance u = pol.decide(in);

    if (u.kind == ProgressKind::Silent || u.text.empty()) {
        p.decision = AckDecision::Silent;
        p.reason = "utterance policy chose silence";
        return p;
    }

    p.decision = AckDecision::Acknowledge;
    p.text = u.text;
    p.reason = "slow task (" + std::to_string(p.expected_ms) + "ms), acknowledge";
    return p;
}

// ========== 轮次闸门 ==========

int FastResponseLayer::acks_in_turn() const {
    return acks_in_turn_.load();
}

void FastResponseLayer::begin_turn() {
    acks_in_turn_.store(0);
}

void FastResponseLayer::notify_spoke() {
    acks_in_turn_.fetch_add(1);
}

void FastResponseLayer::on_interrupt() {
    acks_in_turn_.store(0);
}

}  // namespace voice_agent

// src/orchestrator/smart_turn.cpp
#include "smart_turn.hpp"
#include "util/log.hpp"

namespace voice_agent {

SmartTurn::SmartTurn(Config config) : config_(config) {}
SmartTurn::~SmartTurn() { reset(); }

void SmartTurn::agent_start_speaking() {
    agent_speaking_ = true;
    agent_speech_start_us_ = 0;
    LOG_DEBUG("SmartTurn: agent started speaking");
}

void SmartTurn::agent_stop_speaking() {
    agent_speaking_ = false;
    LOG_DEBUG("SmartTurn: agent stopped speaking");
}

void SmartTurn::user_speech_start(uint64_t timestamp_us) {
    user_speech_start_us_ = timestamp_us;
    user_speaking_ = true;
    LOG_DEBUG("SmartTurn: user started speaking at {}", timestamp_us);
}

void SmartTurn::user_speech_end(uint64_t timestamp_us) {
    if (!user_speaking_) return;

    int duration_ms = static_cast<int>((timestamp_us - user_speech_start_us_) / 1000);
    user_speaking_ = false;
    LOG_DEBUG("SmartTurn: user spoke for {}ms", duration_ms);
}

TurnDecision SmartTurn::evaluate_barge_in(int duration_ms, float coherence) {
    // Agent 没有在说话，不涉及打断
    if (!agent_speaking_.load()) {
        return TurnDecision::AgentYield;
    }

    // 时长过短（< barge_in_min_ms）→ 忽略，可能是噪音或语气词
    if (duration_ms < config_.barge_in_min_ms) {
        LOG_DEBUG("SmartTurn: ignore short speech {}ms < {}ms",
                  duration_ms, config_.barge_in_min_ms);
        return TurnDecision::AgentKeep;
    }

    // 用户说话过长（> backchannel_max_ms）且 Agent 在说话 → 强制让出
    if (duration_ms > config_.backchannel_max_ms) {
        LOG_INFO("SmartTurn: user spoke {}ms > {}ms, AGENT YIELD",
                 duration_ms, config_.backchannel_max_ms);
        ++user_speech_count_;
        return TurnDecision::AgentYield;
    }

    // 时长在 [barge_in_min_ms, backchannel_max_ms] 范围
    // 语义一致性高 → 让出（用户说的和 Agent 说的一致）
    if (coherence > config_.coherence_max) {
        LOG_INFO("SmartTurn: high coherence {:.2f} > {:.2f}, AGENT YIELD",
                 coherence, config_.coherence_max);
        ++user_speech_count_;
        return TurnDecision::AgentYield;
    }

    // 语义一致性低 + 适中时长 → 保持话轮
    LOG_DEBUG("SmartTurn: low coherence {:.2f}, AGENT KEEP", coherence);
    return TurnDecision::AgentKeep;
}

void SmartTurn::reset() {
    agent_speaking_ = false;
    user_speaking_ = false;
    agent_speech_start_us_ = 0;
    user_speech_start_us_ = 0;
    user_speech_count_ = 0;
}

}  // namespace voice_agent

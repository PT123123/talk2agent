// src/orchestrator/smart_turn.hpp
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace voice_agent {

// ========== Smart Turn 轮次判定 ==========
// 策略：基于 VAD 时长 + 内容特征判断是否让出话轮
//
// 让出话轮的信号：
//   1. 用户语音超过 backchannel_max_ms（~600ms）→ 可能说完
//   2. 语音自然停顿 > 350ms + 语句完整 → 让出
//   3. 用户主动提问或请求 → 让出
//
// 保持话轮（要求用户打断）：
//   1. 用户语音 < 160ms → 可能是语气词，继续
//   2. 用户语音 < barge_in_min_ms → 不响应打断
//   3. Agent 正在生成中 → 保持话轮

enum class TurnDecision {
    AgentKeep,    // Agent 保持话轮（忽略打断）
    AgentYield,   // Agent 让出话轮（接受打断）
    AgentWait,    // 等待更多信号
};

struct SmartTurnConfig {
    int barge_in_min_ms = 160;
    int backchannel_max_ms = 600;
    float coherence_max = 0.6f;
};

class SmartTurn {
public:
    using Config = SmartTurnConfig;

    explicit SmartTurn(Config config = {});
    ~SmartTurn();

    // 不可复制
    SmartTurn(const SmartTurn&) = delete;
    SmartTurn& operator=(const SmartTurn&) = delete;

    // Agent 开始说话
    void agent_start_speaking();

    // Agent 停止说话
    void agent_stop_speaking();

    // 用户语音开始
    void user_speech_start(uint64_t timestamp_us);

    // 用户语音结束（检测到用户说完一句话）
    void user_speech_end(uint64_t timestamp_us);

    // 评估用户打断意图
    // duration_ms: 用户连续说话的时长
    // coherence: 与前文的语义一致性（0~1）
    TurnDecision evaluate_barge_in(int duration_ms, float coherence);

    // 当前说话者
    bool is_agent_speaking() const { return agent_speaking_; }
    bool is_user_speaking() const { return user_speaking_; }

    void reset();

private:
    Config config_;
    std::atomic<bool> agent_speaking_{false};
    std::atomic<bool> user_speaking_{false};
    uint64_t agent_speech_start_us_{0};
    uint64_t user_speech_start_us_{0};
    int user_speech_count_{0};   // 打断尝试次数
};

}  // namespace voice_agent

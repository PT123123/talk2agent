// src/orchestrator/semantic_turn.hpp
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace voice_agent {

// ========== 语义完备性 ==========
// 对标 OpenAI semantic_vad 的判定核心：用户说的话**语义上是否说完**，
// 而不只是"静音了多久"。
//
// 关键洞察（业界共识）：静音时长是**代理指标**，不是目标。
//   - "我想问一下...(停顿)...那个项目"  停顿 600ms，但显然没说完
//   - "好的"                              停顿 300ms，但确实说完了
// 纯时长阈值在前者会抢话，在后者会慢半拍。语义判断能同时解决两端。

enum class CompletionHint {
    Incomplete,   // 明显没说完：结尾是连接词/助词/省略号
    LikelyDone,   // 大概率说完：完整句 + 句末标点
    Uncertain     // 说不准
};

// ========== Eagerness（对标 OpenAI 的 low/medium/high）============
// OpenAI 官方：low/medium/high 的最大超时分别是 8s/4s/2s。
// 中文场景应该偏向 low —— 中文是"话题在前、评论在后"的结构，
// 决定性信息常在句末（"这个方案的**优点**是…"），中等急切度会在中途插话。
enum class Eagerness {
    Low,      // 最保守，等更久（中文推荐）
    Medium,
    High      // 最激进，容易抢话
};

struct EagernessConfig {
    const char* name;
    int min_wait_ms;    // 说完后至少等这么久（给用户改口的机会）
    int max_wait_ms;    // 语义判断不确定时的上限
    int doubt_bonus_ms; // 判定"可能没说完"时额外等的时长
};

inline const EagernessConfig eagerness_config(Eagerness e) {
    switch (e) {
        case Eagerness::Low:    return {"low", 500, 8000, 1500};
        case Eagerness::Medium: return {"medium", 350, 4000, 800};
        case Eagerness::High:   return {"high", 200, 2000, 300};
    }
    return {"medium", 350, 4000, 800};
}

// ========== SemanticTurnDetector ==========
// 规则版（零模型开销）。R9b 的目标是先把"语义参与判断"这条通路建起来 ——
// 以后可以换成 LiveKit Turn Detector（135M 文本模型，~50ms 推理）
// 或 Pipecat Smart Turn v3（ONNX CPU），接口不变。
class SemanticTurnDetector {
public:
    struct Config {
        Eagerness eagerness{Eagerness::Low};   // 中文默认 low
        // 句子很短时直接判定说完（"嗯"、"好"），不做语义分析
        int short_utterance_chars{6};
    };

    explicit SemanticTurnDetector(Config cfg = {}) : cfg_(cfg) {}

    // 判断用户这句话的语义完备性
    CompletionHint analyze(const std::string& text) const;

    // 综合判定：语义 + 已静音时长 → 是否该让出话轮
    // silence_ms: VAD 检测到静音的时长
    // 返回 true = 该让出话轮（进入 LLM 轮次）
    bool should_yield(const std::string& text, int silence_ms) const;

    // 当前的让出决策（供 trace 记录）
    struct Decision {
        bool yield{false};
        CompletionHint hint{CompletionHint::Uncertain};
        int waited_ms{0};
        std::string reason;
    };
    Decision last_decision() const;

    void set_eagerness(Eagerness e) { cfg_.eagerness = e; }
    Eagerness eagerness() const { return cfg_.eagerness; }

private:
    Config cfg_;
    mutable std::mutex mutex_;
    mutable Decision last_;
};

}  // namespace voice_agent

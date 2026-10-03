// src/orchestrator/interruption_truncation.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace voice_agent {

// ========== 打断截断结果 ==========
// 对标 OpenAI 的 conversation.item.truncate(audio_end_ms) 与 Azure 的
// auto_truncate + appended_text_after_truncation。
//
// 为什么必须做：TTS 合成速度远快于播放速度。用户打断时，模型往往已经
// 生成了完整回答并写进了对话历史。如果不截断，下一轮模型会**以为那些
// 没播出来的内容也说过** —— 于是：
//   - 跳过用户其实没听到的解释
//   - 说"正如我刚才说的……"而用户压根没听到
// 这就是 context desync。业界把它列为"从 demo 到生产最普遍的第一个 bug"。
struct TruncationResult {
    // 保留（用户实际听到）的文本 —— 按播放比例切
    std::string played_text;
    // 被丢弃（用户没听到）的文本
    std::string dropped_text;
    // 播放比例 0~1
    double played_ratio{1.0};
    // 是否发生了截断（没播完就被打断）
    bool truncated{false};

    bool empty() const { return played_text.empty() && dropped_text.empty(); }
};

// ========== 打断截断器 ==========
// 纯逻辑，无副作用，便于精确单测。
//
// 精度说明：我们按"字符比例"切分，误差在句级。若引擎能给出
// 采样级对齐（TTS 的 word timestamp），可进一步精确到词。
class InterruptionTruncation {
public:
    struct Config {
        // 播放比例低于此值就认为"几乎没播出"，整段丢弃
        double min_played_ratio{0.02};
        // 在保留文本末尾追加的标记（对应 Azure 的 appended_text_after_truncation）。
        // 告诉模型"你被用户打断了"，否则它会困惑于为什么自己没有说完。
        std::string interrupt_marker{"（被用户打断）"};
        // 是否追加标记
        bool append_marker{true};
    };

    explicit InterruptionTruncation(Config cfg = {}) : cfg_(cfg) {}

    // 按播放比例截断
    // full_text: 模型本轮生成的完整回答
    // played_ratio: AudioRouter::played_ratio()
    TruncationResult truncate(const std::string& full_text, double played_ratio) const;

    // 按已播出的段数精确截断（更准 —— 我们有段级信息）
    // played_segments: 已播出的 SpeechSegment 文本
    // all_segments:   本轮全部段的文本
    TruncationResult truncate_by_segments(
        const std::vector<std::string>& played_segments,
        const std::vector<std::string>& all_segments) const;

    // 构造给模型看的"被打断"说明（写进 system prompt 的 P2 层）
    // 返回空串表示不需要（没截断时）
    std::string build_interrupt_note(const TruncationResult& r) const;

private:
    Config cfg_;
};

}  // namespace voice_agent

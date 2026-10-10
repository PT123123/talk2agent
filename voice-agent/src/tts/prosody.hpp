// src/tts/prosody.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace voice_agent {

// ========== 韵律连续变量 ==========
// 关键设计：**不要用 happy/sad/angry 这种粗标签**。它们无法表达
// "用户有点烦但我理解"这种真实状态，而且换 TTS 时全要重做映射。
// 拆成连续变量后，换引擎只需要各自解释这 6 个维度。
struct Prosody {
    float energy{0.5f};      // 力度：0=气若游丝 1=斩钉截铁
    float warmth{0.5f};      // 温度：0=冷淡 1=亲和
    float certainty{0.6f};   // 确定：0=犹疑 1=笃定
    float urgency{0.3f};     // 紧迫：0=从容 1=急
    float pace{0.5f};        // 语速基线：0=慢 1=快
    float pause_density{0.5f}; // 停顿密度：0=连贯 1=句读分明

    bool operator==(const Prosody& o) const;
};

// 情绪 → 韵律基线。刻意做成"小幅偏移"而不是覆盖：
// base 是当前段的基线，情绪只做微调，避免"每句话音色突变"。
Prosody prosody_for_emotion(const std::string& emotion, Prosody base = {});

// ========== 语音段 ==========
// 一段 = 一个可独立合成的最小单位。
// 粒度取"句"而非"词"：逐 token 直灌会让 TTS 总耗时暴增
// （SAPI 一次 Speak 会清空当前读本，逐字喂会互相打断只读几字）。
struct SpeechSegment {
    std::string text;

    // 韵律（0~1 连续变量，映射到引擎的具体参数）
    Prosody prosody;

    // 句首/句尾静音（毫秒）
    int pause_before_ms{0};
    int pause_after_ms{0};

    // 强调片段（用于重音）
    std::vector<std::string> emphasis;

    // 已播出的段不会被修订（见 Response Revision）
    bool played{false};

    // 时长估计（毫秒），供 Response Planner 判断是否还来得及
    int estimated_ms{0};
};

// ========== ProsodyPlanner ==========
// 决定"怎么说"。与"说什么"（LLM 的事）严格分离 ——
// 这样换 TTS 不用改 Agent，换 Agent 不用改韵律。
class ProsodyPlanner {
public:
    struct Config {
        // 是否按标点自动切分
        bool auto_split{true};
        // 单段最大字符数（超过则在次级标点处再切）
        int max_segment_chars{120};
        // 中文语速基线：chars per second
        float base_cps{6.0f};
        // 情绪识别置信度阈值：低于此值不改变韵律
        float emotion_threshold{0.5f};
    };

    explicit ProsodyPlanner(Config cfg = {}) : cfg_(cfg) {}

    // 把一段文本切成若干句子（只切分，不加韵律）。
    // hold_tail=true（流式场景）：末尾没有终止标点的半句不返回，
    // 留在调用方的累积缓冲里等下一批 token；"返回各段的字节长度之和"
    // 即本次已消费的字节数。false（整段一次性规划）：半句也作为最后一段返回。
    // 切分按 UTF-8 码点进行 —— 按单字节匹配标点会把"呀/怎/一"等
    // 编码字节与标点重叠的汉字切碎。
    static std::vector<std::string> split_sentences(const std::string& text,
                                                    int max_chars = 120,
                                                    bool hold_tail = false);

    // 为单个句子生成 SpeechSegment。
    // emotion 为空表示中性；intensity 0~1 表示情绪强度。
    SpeechSegment plan_segment(const std::string& sentence,
                              const std::string& emotion = {},
                              float intensity = 0.0f) const;

    // 规划整段回复。context 用于判断"这是回答的开头/结尾"，
    // 因为接在别人话后面和自成一段的韵律不同。
    std::vector<SpeechSegment> plan(const std::string& text,
                                    const std::string& emotion = {},
                                    float intensity = 0.0f,
                                    bool is_turn_head = true,
                                    bool is_turn_tail = true) const;

    // 估计某段文本的合成后时长（毫秒）。用于 Response Planner
    // 判断"这句念完用户还在等吗"。
    int estimate_duration_ms(const std::string& text, Prosody p = {}) const;

    // 韵律 → 引擎语速倍率。
    // rate = 1.0 是基准；pace 高 + urgency 高会 > 1.0。
    // 范围钳制在 [0.6, 1.8]，超出后听感反而变差。
    static float rate_from_prosody(Prosody p);

    // 韵律 → 停顿（毫秒）。pause_density 高 + certainty 低 → 停顿更明显
    // （不确定的话迟疑更久）。
    static int pause_after_ms_from_prosody(Prosody p);

private:
    Config cfg_;
};

}  // namespace voice_agent

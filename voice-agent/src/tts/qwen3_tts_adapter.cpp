// src/tts/qwen3_tts_adapter.cpp
#include "tts/qwen3_tts_adapter.hpp"
#include "tts/tts.hpp"
#include "util/log.hpp"

#include <string>
#include <vector>

namespace voice_agent {

Qwen3Controls map_prosody_for_qwen3(const Prosody& p) {
    Qwen3Controls c;

    // instruction：把连续维度翻译成自然语言风格提示。
    // 顺序固定，便于单测按关键词断言。多个维度用 "、" 连接。
    std::vector<std::string> cues;

    // 温度（warmth）：高 = 温柔亲切，低 = 冷静克制
    if (p.warmth >= 0.6f)       cues.emplace_back("温柔亲切");
    else if (p.warmth <= 0.4f)  cues.emplace_back("冷静克制");

    // 力度 + 紧迫：高 = 充满活力 / 急切
    if (p.energy >= 0.6f && p.urgency >= 0.6f)
        cues.emplace_back("充满活力");
    else if (p.urgency >= 0.7f)
        cues.emplace_back("急切");

    // 确定：低 = 带着些许迟疑
    if (p.certainty <= 0.4f)     cues.emplace_back("带着些许迟疑");

    // 停顿密度高 = 叙述从容（句读分明）
    if (p.pause_density >= 0.7f) cues.emplace_back("叙述从容");

    if (cues.empty()) {
        c.instruction = "自然清晰";
    } else {
        std::string s;
        for (size_t i = 0; i < cues.size(); ++i) {
            if (i) s += "、";
            s += cues[i];
        }
        c.instruction = s;
    }

    // speed：语速倍率（与模型引擎共用同一语义映射）
    c.speed = ProsodyPlanner::rate_from_prosody(p);

    // voice：默认空（后端默认音色），克隆场景由上层注入
    c.voice.clear();
    return c;
}

void Qwen3TtsAdapter::apply_prosody(const Prosody& p) {
    controls_ = map_prosody_for_qwen3(p);
    if (!tts_) return;

    // 把控制写进 TTS 的 bridge 通道 —— instruction / voice / speed 在这里
    // 被真正消费（Python bridge 会把 instruct= 传给 Qwen3-TTS）。
    // 这一步之前这些值只是"算好放着"，回退模式下只有 speed 有效果。
    TtsBridgeControls bc;
    bc.instruction = controls_.instruction;
    bc.voice       = controls_.voice;
    bc.speed       = controls_.speed;
    tts_->set_bridge_controls(bc);

    // SAPI/模型引擎路径仍然靠 set_speed（回退时让它生效）。
    tts_->set_speed(controls_.speed);

    LOG_DEBUG("Qwen3TtsAdapter: instruction='{}' speed={:.2f} bridge={}",
              controls_.instruction, controls_.speed,
              tts_->uses_bridge() ? "on" : "off (回退发声)");
}

void Qwen3TtsAdapter::synthesize(const SpeechSegment& seg, TTSCallback callback) {
    if (!tts_ || seg.text.empty()) {
        if (callback) callback(nullptr, 0, true);
        return;
    }
    apply_prosody(seg.prosody);
    // 实际发声：
    //   bridge 可用 → Python 侧 Qwen3-TTS 消费 instruction（风格真正生效）
    //   bridge 不可用 → TTS 内部自动回退到底层声学引擎（仅 speed 生效）
    // 两条路径对上层完全一样，Agent 逻辑无需区分。
    tts_->synthesize_stream(seg.text, std::move(callback));
}

}  // namespace voice_agent

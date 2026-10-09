// src/tts/chatterbox_adapter.cpp
#include "tts/chatterbox_adapter.hpp"
#include "tts/tts.hpp"
#include "util/log.hpp"

#include <algorithm>

namespace voice_agent {

ChatterboxControls map_prosody_for_chatterbox(const Prosody& p) {
    ChatterboxControls c;

    // exaggeration：能量 + 温度共同决定情绪强度。
    // 高能量 / 高温度 -> 更外放（情绪更强）。
    float ex = 0.25f + 0.50f * p.energy + 0.25f * p.warmth;
    c.exaggeration = std::clamp(ex, 0.0f, 1.0f);

    // cfg_weight：确定（稳重）高 -> 更克制清晰；紧迫（想快）高 -> 更松弛。
    // 推荐区间 0.1~1，钳制到 [0.1, 1.0]。
    float cfg = 0.55f + 0.35f * (p.certainty - 0.5f)
                      - 0.35f * (p.urgency - 0.3f);
    c.cfg_weight = std::clamp(cfg, 0.1f, 1.0f);

    // speed：语速倍率（与模型引擎共用同一语义映射）
    c.speed = ProsodyPlanner::rate_from_prosody(p);
    return c;
}

void ChatterboxAdapter::apply_prosody(const Prosody& p) {
    controls_ = map_prosody_for_chatterbox(p);
    if (!tts_) return;

    // exaggeration / cfg_weight 写进 bridge 通道，由 Python 侧
    // Chatterbox.generate(exaggeration=..., cfg_weight=...) 真正消费。
    // 没有这一步，这两个旋钮就只是"算好放着"，回退发声时完全无效。
    TtsBridgeControls bc;
    bc.exaggeration = controls_.exaggeration;
    bc.cfg_weight   = controls_.cfg_weight;
    bc.speed        = controls_.speed;
    tts_->set_bridge_controls(bc);

    tts_->set_speed(controls_.speed);

    LOG_DEBUG("ChatterboxAdapter: exaggeration={:.2f} cfg_weight={:.2f} "
              "speed={:.2f} bridge={}",
              controls_.exaggeration, controls_.cfg_weight, controls_.speed,
              tts_->uses_bridge() ? "on" : "off (回退发声)");
}

void ChatterboxAdapter::synthesize(const SpeechSegment& seg, TTSCallback callback) {
    if (!tts_ || seg.text.empty()) {
        if (callback) callback(nullptr, 0, true);
        return;
    }
    apply_prosody(seg.prosody);
    // bridge 可用 → exaggeration / cfg_weight 真正生效；
    // 不可用 → TTS 内部回退底层声学引擎。两条路径上层无感。
    tts_->synthesize_stream(seg.text, std::move(callback));
}

}  // namespace voice_agent

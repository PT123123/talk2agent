// src/tts/tts_adapter.cpp
#include "tts_adapter.hpp"
#include "tts/tts.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cmath>

namespace voice_agent {

// ========== 韵律 → 引擎参数 ==========

EngineParams map_prosody_for_model(const Prosody& p) {
    // Kokoro/Piper 只有语速。把能表达的都压进 speed：
    // 能量高的句子略快（急迫），pause_density 通过段后静音表达。
    EngineParams e;
    float rate = 0.75f + 0.45f * p.pace + 0.15f * p.urgency
                 - 0.10f * (1.0f - p.certainty);
    // 能量高也推一点速度：听感上"有劲"和"快"很难分开，
    // 而让模型真的变响在不同后端上不可靠。
    rate += 0.06f * (p.energy - 0.5f);
    e.speed = std::clamp(rate, 0.6f, 1.8f);
    e.volume = 1.0f;    // 这两个参数该后端不支持
    e.pitch = 1.0f;
    e.pause_ms = ProsodyPlanner::pause_after_ms_from_prosody(p);
    return e;
}

EngineParams map_prosody_for_sapi(const Prosody& p) {
    // SAPI 的 Rate 是 -10..10 的整数档位，Volume/Pitch 是百分比。
    // 这里转成倍率再由调用方换算，避免各引擎参数单位混在一起。
    EngineParams e;
    float rate = 0.75f + 0.45f * p.pace + 0.15f * p.urgency
                 - 0.10f * (1.0f - p.certainty);
    e.speed = std::clamp(rate, 0.6f, 1.8f);

    // 音量：energy 主控，warmth 微调。范围压到 [0.7, 1.3] ——
    // 超过这个范围 SAPI 的音量会明显失真。
    e.volume = std::clamp(0.85f + 0.30f * p.energy + 0.08f * (p.warmth - 0.5f),
                          0.7f, 1.3f);

    // 音调：certainty 高略微上扬（笃定），低则略降（迟疑）。
    // 变化幅度刻意小 —— 超过 ±5% 就会显得"阴阳怪气"。
    e.pitch = std::clamp(1.0f + 0.05f * (p.certainty - 0.5f)
                             + 0.03f * (p.energy - 0.5f),
                         0.95f, 1.05f);

    e.pause_ms = ProsodyPlanner::pause_after_ms_from_prosody(p);
    return e;
}

// ========== ModelTtsAdapter ==========

void ModelTtsAdapter::apply_prosody(const Prosody& p) {
    if (!tts_) return;
    const EngineParams e = map_prosody_for_model(p);
    tts_->set_speed(e.speed);
}

void ModelTtsAdapter::synthesize(const SpeechSegment& seg, TTSCallback callback) {
    if (!tts_ || seg.text.empty()) {
        if (callback) callback(nullptr, 0, true);
        return;
    }
    apply_prosody(seg.prosody);
    tts_->synthesize_stream(seg.text, std::move(callback));
}

// ========== SapiTtsAdapter ==========

void SapiTtsAdapter::apply_prosody(const Prosody& p) {
    if (!tts_) return;
    const EngineParams e = map_prosody_for_sapi(p);
    tts_->set_speed(e.speed);
    // SAPI 的音量/音调通过配置面生效（set_speed 是唯一运行时接口，
    // 音量音调在 TTSConfig 里）。这里只记日志说明当前生效的是语速，
    // 避免"以为改了音量其实没改"的隐性 bug。
    static bool warned = false;
    if (!warned && (std::fabs(e.volume - 1.0f) > 0.01f ||
                   std::fabs(e.pitch - 1.0f) > 0.001f)) {
        LOG_DEBUG("SAPI adapter: volume={:.2f} pitch={:.3f} not runtime-adjustable, "
                  "speed={:.2f} applied", e.volume, e.pitch, e.speed);
        warned = true;
    }
}

void SapiTtsAdapter::synthesize(const SpeechSegment& seg, TTSCallback callback) {
    if (!tts_ || seg.text.empty()) {
        if (callback) callback(nullptr, 0, true);
        return;
    }
    apply_prosody(seg.prosody);
    // SAPI 必须整段送入：一次 Speak 会清空当前读本，
    // 逐字喂会互相打断只读几个字。
    tts_->synthesize_stream(seg.text, std::move(callback));
}

}  // namespace voice_agent

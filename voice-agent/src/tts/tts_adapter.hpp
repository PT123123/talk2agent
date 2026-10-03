// src/tts/tts_adapter.hpp
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/cancel_token.hpp"
#include "tts/prosody.hpp"
#include "tts/tts.hpp"        // TTSCallback

namespace voice_agent {

// ========== TTS 适配器 ==========
// 抽象"一个 TTS 后端如何执行 SpeechSegment"。
//
// 为什么要这层：SpeechSegment 里的 Prosody 是**连续的抽象量**
// （energy/warmth/pace/...），而每个引擎能表达的参数完全不同：
//   - Kokoro  只有语速（speak rate），没有独立的 energy/warmth
//   - SAPI    有 Rate/Volume/Pitch，但语义是 SAPI 自己的百分比
//   - Qwen3-TTS 有 instruction 控制（"用兴奋的语气说"）
//   - Chatterbox 有 exaggeration / cfg_weight
// 这层负责把统一语义映射到各引擎的具体参数，**换引擎不改 Agent 逻辑**。
class ITtsAdapter {
public:
    virtual ~ITtsAdapter() = default;

    virtual std::string name() const = 0;

    // 把韵律应用到引擎（语速/音量/音调）。应在合成前调用。
    virtual void apply_prosody(const Prosody& p) = 0;

    // 合成一段。callback 收到音频；is_last=true 表示该段结束。
    virtual void synthesize(const SpeechSegment& seg, TTSCallback callback) = 0;

    // 该引擎能否表达完整的韵律维度。false 时 apply_prosody 只能映射子集，
    // 上层可据此决定是否退化到中性韵律。
    virtual bool supports_full_prosody() const { return false; }
};

// ========== 现有引擎适配器 ==========

// Kokoro / Piper：只有语速。energy 映射为语速微调（能量高的句子略快）。
class ModelTtsAdapter : public ITtsAdapter {
public:
    explicit ModelTtsAdapter(TTS* tts) : tts_(tts) {}
    std::string name() const override { return "model"; }
    void apply_prosody(const Prosody& p) override;
    void synthesize(const SpeechSegment& seg, TTSCallback callback) override;
    bool supports_full_prosody() const override { return false; }

private:
    TTS* tts_;
};

// Windows SAPI：Rate/Volume/Pitch 三个百分比参数，映射能力较强。
// 注意 SAPI 一次 Speak 会清空当前读本，因此必须整段送入。
class SapiTtsAdapter : public ITtsAdapter {
public:
    explicit SapiTtsAdapter(TTS* tts) : tts_(tts) {}
    std::string name() const override { return "sapi"; }
    void apply_prosody(const Prosody& p) override;
    void synthesize(const SpeechSegment& seg, TTSCallback callback) override;
    // SAPI 能表达 pace + energy + warmth(音量) 三维，算较强
    bool supports_full_prosody() const override { return true; }

private:
    TTS* tts_;
};

// ========== 韵律 → 引擎参数映射（纯函数，可单测）============
struct EngineParams {
    float speed{1.0f};     // 语速倍率
    float volume{1.0f};    // 音量倍率
    float pitch{1.0f};     // 音调倍率
    int    pause_ms{0};    // 段后静音
};

EngineParams map_prosody_for_model(const Prosody& p);
EngineParams map_prosody_for_sapi(const Prosody& p);

}  // namespace voice_agent

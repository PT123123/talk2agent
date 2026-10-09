// src/tts/chatterbox_adapter.hpp
#pragma once
#include <string>
#include "tts/tts_adapter.hpp"

namespace voice_agent {

// ========== Chatterbox 适配器（升级计划 §24） ==========
//
// Chatterbox（resemble-ai，PyTorch）暴露 exaggeration（情绪强度0~1）与
// cfg_weight（推荐 0.1~1，控制"稳重/清晰"程度：越高越克制、越低越松弛）
// 两个旋钮，外加 speed。
//
// 本适配器把统一韵律语义映射到这两个旋钮 + speed，经 TTS 的 bridge 通道
// 交给本地 Python 推理进程（scripts/tts_bridge_server.py →
// chatterbox.generate(exaggeration=..., cfg_weight=...)）。
//
// 诚实边界：bridge 不可用（未启动 / 缺 chatterbox-tts / 没权重）时
// TTS 自动回退到底层声学引擎，此时 exaggeration / cfg_weight 无处可去、
// 只有 speed 有效果。桥接代码已就位，缺的只是 Python 环境与权重。
struct ChatterboxControls {
    float exaggeration{0.5f};   // 情绪强度（0~1）
    float cfg_weight{0.5f};    // 克制/清晰程度（0.1~1）
    float speed{1.0f};         // 语速倍率（0.6~1.8）
};

// 纯函数：韵律 → Chatterbox 控制（可单测）。
ChatterboxControls map_prosody_for_chatterbox(const Prosody& p);

class ChatterboxAdapter : public ITtsAdapter {
public:
    explicit ChatterboxAdapter(TTS* tts) : tts_(tts) {}
    std::string name() const override { return "chatterbox"; }

    void apply_prosody(const Prosody& p) override;
    void synthesize(const SpeechSegment& seg, TTSCallback callback) override;

    // Chatterbox 通过 exaggeration 表达情绪强弱，比纯语速后端强。
    bool supports_full_prosody() const override { return true; }

    const ChatterboxControls& last_controls() const { return controls_; }

private:
    TTS* tts_;
    ChatterboxControls controls_;
};

}  // namespace voice_agent

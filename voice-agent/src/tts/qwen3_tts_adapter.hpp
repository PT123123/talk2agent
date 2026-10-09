// src/tts/qwen3_tts_adapter.hpp
#pragma once
#include <string>
#include "tts/tts_adapter.hpp"

namespace voice_agent {

// ========== Qwen3-TTS 适配器（升级计划 §24） ==========
//
// Qwen3-TTS（QwenLM，PyTorch）的核心控制是"自然语言 instruction"
// + 可选的 voice cloning / voice design，而不是 rate/pitch 那种数值旋钮。
// 这个适配器把统一韵律语义（6 个连续维度）映射成一段 instruction 文本
// + speed，再经 TTS 的 bridge 通道交给本地 Python 推理进程
// （scripts/tts_bridge_server.py → qwen_tts.Qwen3TTSModel.generate_custom_voice）。
//
// 两条发声路径（对上层完全一致）：
//   1. bridge 可用 → instruction / voice / speed 真正被模型消费，
//      这是"跑起来"的状态。
//   2. bridge 不可用（未启动 / 缺包 / 没权重）→ TTS 自动回退到底层
//      声学引擎（SAPI/Kokoro），此时只有 speed 有效果。
//
// 诚实边界：路径 1 需要真的部署 Python 推理环境（pip install qwen-tts +
// 下载权重）。本机 Intel Arc 2GB 显存下 0.6B fp32（约 2.5GB 权重）
// 需要 fp16 + CPU offload 才能装下——这一层是环境问题，不是代码问题。
struct Qwen3Controls {
    std::string instruction;   // 风格指令（自然语言）
    float speed{1.0f};         // 语速倍率（0.6~1.8）
    std::string voice;         // 音色（克隆/设计），默认空 = 引擎默认
};

// 纯函数：韵律 → Qwen3-TTS 控制（可单测）。
Qwen3Controls map_prosody_for_qwen3(const Prosody& p);

class Qwen3TtsAdapter : public ITtsAdapter {
public:
    explicit Qwen3TtsAdapter(TTS* tts) : tts_(tts) {}
    std::string name() const override { return "qwen3tts"; }

    void apply_prosody(const Prosody& p) override;
    void synthesize(const SpeechSegment& seg, TTSCallback callback) override;

    // Qwen3-TTS 能表达 warmth/certainty/energy（via instruction + voice），
    // 比 Kokoro/Piper 强，标记为完全支持。
    bool supports_full_prosody() const override { return true; }

    // 供测试 / 后端桥接读取最近一次算出的控制。
    const Qwen3Controls& last_controls() const { return controls_; }

private:
    TTS* tts_;
    Qwen3Controls controls_;
};

}  // namespace voice_agent

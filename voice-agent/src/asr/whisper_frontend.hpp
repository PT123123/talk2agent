// src/asr/whisper_frontend.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace voice_agent {

// ========== Whisper 前端：16kHz PCM → log-mel 频谱 ==========
// 复刻 OpenAI Whisper 的 LogMelSpectrogram：
//  - n_fft=400 (25ms) / hop=160 (10ms)
//  - 80 mel bins, fmin=0, fmax=8000, Slaney 归一化 mel 缩放
//  - log10 + 动态范围 clamp + 归一化，填充到固定长度交予模型
struct WhisperFrontend {
    static constexpr int kSampleRate = 16000;
    // 与 OpenAI whisper 官方前端对齐：N_FFT=400 / HOP=160 / 80 mel / fmax=8000
    static constexpr int kNfft = 400;
    static constexpr int kHop = 160;
    static constexpr int kNmels = 80;
    static constexpr int kMaxFrames = 3000;  // 30 秒 @ 10ms
    static constexpr int kMaxMelSamples = kNmels * kMaxFrames;

    mutable bool initialized_{false};
    mutable std::vector<float> filterbank_;   // kNmels x (kNfft/2+1)

    void init() const;
    void ensure_initialized() const;

    // 输入 int16 单声道 16kHz PCM→输出 float[kNmels x frames] log-mel（尚未 pad）
    // 调用方负责 pad 到 kNmels*kMaxFrames 与归一化
    std::vector<float> compute(const int16_t* pcm, size_t frames) const;
};

}  // namespace voice_agent
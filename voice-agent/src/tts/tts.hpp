// src/tts/tts.hpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "core/types.hpp"
#include "core/cancel_token.hpp"

namespace voice_agent {

// TTS 配置
struct TTSConfig {
    std::string model_path;      // 模型路径
    std::string voice_path;       // 音色路径
    float speed = 1.0f;          // 语速
    float pitch = 1.0f;          // 音调
    float volume = 1.0f;         // 音量
    int sample_rate = 24000;      // 输出采样率
    std::string lang = "zh";      // 语言
};

// TTS 音频回调
using TTSCallback = std::function<void(const int16_t* audio, size_t frames, bool is_last)>;

class TTS {
public:
    TTS();
    ~TTS();

    // 初始化 TTS
    bool initialize(const TTSConfig& config);

    // 合成语音（阻塞）
    std::vector<int16_t> synthesize(const std::string& text);

    // 流式合成
    void synthesize_stream(const std::string& text, TTSCallback callback);

    // 中断合成
    void stop();

    // 设置取消令牌
    void set_cancel_token(std::shared_ptr<CancelToken> token);

    // 是否正在合成
    bool is_synthesizing() const { return synthesizing_; }

    // 获取配置
    const TTSConfig& config() const { return config_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TTSConfig config_;
    std::atomic<bool> synthesizing_{false};
    std::shared_ptr<CancelToken> cancel_token_;
};

// 创建 TTS 实例
std::unique_ptr<TTS> create_kokoro_tts();

}  // namespace voice_agent

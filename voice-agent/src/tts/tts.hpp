// src/tts/tts.hpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "core/types.hpp"
#include "core/cancel_token.hpp"

namespace voice_agent {

class SapiSpeaker;

// TTS 配置
struct TTSConfig {
    std::string engine = "simple";  // "simple"=Windows 系统语音(SAPI，免模型/流式默认)；"kokoro"=本地 Kokoro 模型
    std::string model_path;      // 模型路径（Kokoro: model.onnx）
    std::string voice_path;       // 音色路径（Kokoro: voices.bin）
    std::string tokens_path;      // token 表（Kokoro: tokens.txt）
    std::string data_dir;         // espeak-ng-data 目录
    std::string lexicon;          // 可选词典（Kokoro: lexicon-zh.txt）
    int speaker_id = 45;          // 发音人 ID（Kokoro 多语言：45=zf_xiaobei 中文女声）
    float speed = 1.0f;          // 语速
    float pitch = 1.0f;          // 音调
    float volume = 1.0f;         // 音量
    int sample_rate = 24000;      // 输出采样率
    std::string lang = "zh";      // 语言
    std::string provider = "auto"; // auto: DirectML 可用则 GPU，否则 CPU；可选 dml / cpu
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

    // 是否使用"简单引擎"（Windows 系统语音 SAPI，免模型、实时流式）
    bool simple_engine() const { return simple_engine_; }

    // 简单引擎模式下，朗读结束回调（SAPI 播完一段后触发一次，用于驱动状态机）
    void set_speech_done_callback(std::function<void()> cb);

    // 当前朗读中的系统语音名称（仅简单引擎）
    std::string simple_voice_name() const;

    // 是否运行在真实模型（非 Mock）后端
    bool uses_real_backend() const;

    // 生效推理后端标签："DirectML GPU" / "CPU" / "系统语音(SAPI)" / "Mock"
    std::string provider_label() const;

    // 获取配置
    const TTSConfig& config() const { return config_; }

    // 最近一次合成耗时（毫秒，供调试面板轮询）
    std::atomic<double> last_synthesize_ms{0.0};

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TTSConfig config_;
    std::atomic<bool> synthesizing_{false};
    std::atomic<bool> simple_engine_{false};
    std::shared_ptr<CancelToken> cancel_token_;
};

// 创建 TTS 实例
std::unique_ptr<TTS> create_kokoro_tts();

}  // namespace voice_agent

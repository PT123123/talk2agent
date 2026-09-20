// src/asr/asr.hpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "core/types.hpp"
#include "core/cancel_token.hpp"

namespace voice_agent {

// ASR 识别结果
struct ASRResult {
    std::string text;           // 识别文本
    float confidence = 0.0f;     // 置信度
    bool is_final = false;       // 是否最终结果
    int64_t timestamp_ms = 0;    // 时间戳
};

// ASR 配置
struct ASRConfig {
    std::string model_path;     // 模型路径
    std::string tokens_path;     // 词表路径
    int sample_rate = 16000;     // 采样率
    int num_threads = 4;         // CPU 线程数
    bool use_gpu = true;         // 是否使用 GPU
    std::string provider = "cpu"; // 推理 provider: cpu, cuda, vulkan
};

// ASR 回调
using ASRCallback = std::function<void(const ASRResult& result)>;

class ASR {
public:
    ASR();
    ~ASR();

    // 初始化 ASR 模型
    bool initialize(const ASRConfig& config);

    // 处理音频数据（int16_t PCM）
    void process(const int16_t* pcm, size_t frames);

    // 处理音频数据（float PCM, -1.0~1.0）
    void process_float(const float* pcm, size_t frames);

    // 设置识别结果回调
    void set_callback(ASRCallback callback);

    // 设置取消令牌
    void set_cancel_token(std::shared_ptr<CancelToken> token);

    // 重置识别状态
    void reset();

    // 是否正在识别
    bool is_active() const { return active_; }

    // 获取配置
    const ASRConfig& config() const { return config_; }

private:
    struct Impl;  // NOLINT — pimpl idiom, defined in cpp
    std::unique_ptr<Impl> impl_;
    ASRConfig config_;
    std::atomic<bool> active_{false};
    std::shared_ptr<CancelToken> cancel_token_;
};

// 创建 ASR 实例工厂
std::unique_ptr<ASR> create_sense_voice_asr();
std::unique_ptr<ASR> create_whisper_asr();

}  // namespace voice_agent

// src/asr/sherpa_asr.hpp
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace voice_agent {

// ========== sherpa-onnx 离线中文 ASR（Paraformer-zh / SenseVoice） ==========
// 目录约定（model_dir 直接包含模型与词表）：
//   - Paraformer-zh: model.int8.onnx / model.onnx + tokens.txt，目录名含 "paraformer"
//   - SenseVoice:    model.onnx / model.int8.onnx + tokens.txt，目录名含 "sense"
// 输入 16kHz int16 单声道 PCM，输出识别文本。
class SherpaAsr {
public:
    SherpaAsr();
    ~SherpaAsr();
    SherpaAsr(const SherpaAsr&) = delete;
    SherpaAsr& operator=(const SherpaAsr&) = delete;

    bool load(const std::string& model_dir, int num_threads = 4,
              const std::string& provider = "auto");
    bool loaded() const { return loaded_; }
    std::string last_error() const { return error_; }
    std::string model_name() const;   // "paraformer-zh" / "sense-voice"

    // 生效推理后端："DirectML GPU" / "CPU"
    std::string provider_label() const;

    std::string transcribe(const int16_t* pcm, size_t frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool loaded_{false};
    std::string error_;
    std::string name_;
    std::string provider_ = "cpu";
};

}  // namespace voice_agent

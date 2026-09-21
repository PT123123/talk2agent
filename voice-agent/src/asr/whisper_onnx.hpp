// src/asr/whisper_onnx.hpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace voice_agent {

// ========== Whisper-tiny ONNX 实时/离线 ASR ==========
// encoder_model.onnx + decoder_model.onnx（无 KV-cache，greedy 解码）
// + 自实现 log-mel 前端 + BPE tokenizer（vocab.json + merges.txt）。
// 输入 16kHz int16 单声道 PCM，输出识别文本。
class WhisperOnnx {
public:
    WhisperOnnx();
    ~WhisperOnnx();
    WhisperOnnx(const WhisperOnnx&) = delete;
    WhisperOnnx& operator=(const WhisperOnnx&) = delete;

    // 加载 encoder/decoder 模型 + tokenizer。返回是否成功。
    bool load(const std::string& model_dir);
    bool loaded() const { return loaded_; }
    std::string last_error() const { return error_; }

    // 整段转写：pcm 16kHz int16 → text
    std::string transcribe(const int16_t* pcm, size_t frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool loaded_{false};
    std::string error_;
};

}  // namespace voice_agent
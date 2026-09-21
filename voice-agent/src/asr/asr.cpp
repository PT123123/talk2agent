// src/asr/asr.cpp
#include "asr.hpp"
#include "asr/sherpa_asr.hpp"
#include "asr/whisper_onnx.hpp"
#include "util/log.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace voice_agent {

// ========== ASR 实现（pimpl） ==========
struct ASR::Impl {
    ASRCallback callback;
    std::vector<float> audio_buffer;
    size_t samples_per_chunk = 0;

    // 真实模型后端（sherpa-onnx 中文 / Whisper ONNX）。为空则回退到流式 Mock。
    std::unique_ptr<SherpaAsr> sherpa;
    std::string sherpa_name;
    std::unique_ptr<WhisperOnnx> whisper;
    bool whisper_mode = false;

    // 是否在 model_path 指定目录内发现 Whisper 模型文件
    static bool is_whisper_dir(const std::string& model_path) {
        if (model_path.empty()) return false;
        std::error_code ec;
        return std::filesystem::exists(model_path + "/onnx/encoder_model.onnx", ec);
    }

    // sherpa-onnx 中文模型目录：model.onnx / model.int8.onnx + tokens.txt
    static bool is_sherpa_dir(const std::string& model_path) {
        if (model_path.empty()) return false;
        std::error_code ec;
        return (std::filesystem::exists(model_path + "/model.onnx", ec) ||
                std::filesystem::exists(model_path + "/model.int8.onnx", ec)) &&
               std::filesystem::exists(model_path + "/tokens.txt", ec);
    }

    // Moonshine 目录：经典版 preprocessor+encoder+cached_decoder，或 v2 版 .ort 文件
    static bool is_moonshine_dir(const std::string& model_path) {
        if (model_path.empty()) return false;
        std::error_code ec;
        const bool classic =
            std::filesystem::exists(model_path + "/preprocessor.onnx", ec) &&
            std::filesystem::exists(model_path + "/encoder.onnx", ec) &&
            std::filesystem::exists(model_path + "/tokens.txt", ec);
        const bool v2 =
            std::filesystem::exists(model_path + "/encoder_model.ort", ec) &&
            std::filesystem::exists(model_path + "/decoder_model_merged.ort", ec);
        return classic || v2;
    }

    bool load_model(const ASRConfig& config) {
        // 1) sherpa-onnx Paraformer-zh / SenseVoice（中文优先，新默认）
        if (is_sherpa_dir(config.model_path)) {
            auto s = std::make_unique<SherpaAsr>();
            if (s->load(config.model_path, config.num_threads, config.provider)) {
                sherpa = std::move(s);
                sherpa_name = sherpa->model_name();
                LOG_INFO("ASR: real sherpa-onnx backend loaded ({}) from {}",
                         sherpa_name, config.model_path);
                return true;
            }
            LOG_WARN("ASR: sherpa-onnx load failed ({}), falling back to whisper/mock",
                     s->last_error());
            // 继续尝试 Whisper
        }
        // 1.5) Moonshine（sherpa-onnx 离线，v2 量化包）
        if (is_moonshine_dir(config.model_path)) {
            auto s = std::make_unique<SherpaAsr>();
            if (s->load(config.model_path, config.num_threads, config.provider)) {
                sherpa = std::move(s);
                sherpa_name = sherpa->model_name();
                LOG_INFO("ASR: real Moonshine backend loaded ({}) from {}",
                         sherpa_name, config.model_path);
                return true;
            }
            LOG_WARN("ASR: Moonshine load failed ({}), falling back to whisper/mock",
                     s->last_error());
            // 继续尝试 Whisper
        }
        // 2) Whisper ONNX（旧默认，保留）
        if (is_whisper_dir(config.model_path)) {
            auto w = std::make_unique<WhisperOnnx>();
            if (w->load(config.model_path)) {
                whisper = std::move(w);
                whisper_mode = true;
                LOG_INFO("ASR: real Whisper backend loaded from {}", config.model_path);
                return true;
            }
            LOG_WARN("ASR: Whisper load failed ({}), falling back to mock", w->last_error());
            // 加载失败落到 mock 流式
        }
        LOG_INFO("MockASR: initialized (model={})", config.model_path);
        samples_per_chunk = 16000;  // 1 second @ 16kHz
        return true;
    }

    void recognize(const float* pcm, size_t frames) {
        // Simple energy-based simulation
        float sum = 0;
        for (size_t i = 0; i < frames; ++i) {
            sum += std::abs(pcm[i]);
        }
        float energy = sum / static_cast<float>(frames);

        if (energy > 0.01f && callback) {
            ASRResult result;
            result.text = "[模拟语音输入]";
            result.confidence = 0.85f;
            result.is_final = true;
            result.timestamp_ms = 0;
            callback(result);
        }
    }

    void process_audio(const float* pcm, size_t frames) {
        audio_buffer.insert(audio_buffer.end(), pcm, pcm + frames);
        // 160ms = 2560 samples @ 16kHz
        const size_t min_samples = 2560;
        while (audio_buffer.size() >= min_samples) {
            std::vector<float> chunk(audio_buffer.begin(), audio_buffer.begin() + min_samples);
            audio_buffer.erase(audio_buffer.begin(), audio_buffer.begin() + min_samples);
            recognize(chunk.data(), chunk.size());
        }
    }
};

// ========== ASR 主类实现 ==========

ASR::ASR() = default;
ASR::~ASR() = default;

bool ASR::initialize(const ASRConfig& config) {
    config_ = config;
    impl_ = std::make_unique<Impl>();

    if (!impl_->load_model(config)) {
        LOG_ERROR("ASR: failed to load model");
        return false;
    }

    active_ = true;
    LOG_INFO("ASR: initialized {} Hz, provider={}", config.sample_rate, config.provider);
    return true;
}

void ASR::process(const int16_t* pcm, size_t frames) {
    if (!active_) return;
    std::vector<float> float_audio(frames);
    for (size_t i = 0; i < frames; ++i) {
        float_audio[i] = pcm[i] / 32768.0f;
    }
    process_float(float_audio.data(), frames);
}

void ASR::process_float(const float* pcm, size_t frames) {
    if (!active_ || !impl_) return;
    if (cancel_token_ && cancel_token_->cancelled()) {
        reset();
        return;
    }
    impl_->process_audio(pcm, frames);
}

std::string ASR::transcribe_segment(const int16_t* pcm, size_t frames) {
    if (!active_ || !impl_ || !pcm || frames == 0) return {};

#ifdef USE_SHERPAONNX
    if (impl_->sherpa) {
        const auto t0 = std::chrono::steady_clock::now();
        std::string text = impl_->sherpa->transcribe(pcm, frames);
        last_inference_ms.store(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0)
                .count());
        if (!text.empty() && impl_->callback) {
            ASRResult result;
            result.text = text;
            result.confidence = 0.95f;
            result.is_final = true;
            result.timestamp_ms = 0;
            impl_->callback(result);
        }
        return text;
    }
#endif
#ifdef USE_ONNXRUNTIME
    if (impl_->whisper) {
        const auto t0 = std::chrono::steady_clock::now();
        std::string text = impl_->whisper->transcribe(pcm, frames);
        last_inference_ms.store(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0)
                .count());
        if (!text.empty() && impl_->callback) {
            ASRResult result;
            result.text = text;
            result.confidence = 0.9f;
            result.is_final = true;
            result.timestamp_ms = 0;
            impl_->callback(result);
        }
        return text;
    }
#endif
    // Mock/流式模式：把整段交给识别器
    std::vector<float> f(frames);
    for (size_t i = 0; i < frames; ++i) f[i] = pcm[i] / 32768.0f;
    const auto t0 = std::chrono::steady_clock::now();
    impl_->recognize(f.data(), f.size());
    last_inference_ms.store(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count());
    return {};
}

bool ASR::uses_real_backend() const {
    return impl_ != nullptr && (impl_->sherpa != nullptr || impl_->whisper != nullptr);
}

std::string ASR::provider_label() const {
#ifdef USE_SHERPAONNX
    if (impl_ && impl_->sherpa) return impl_->sherpa->provider_label();
#endif
#ifdef USE_ONNXRUNTIME
    if (impl_ && impl_->whisper) return "CPU";  // Whisper ONNX 为纯 CPU 推理
#endif
    return "Mock";
}

std::string ASR::backend_name() const {
#ifdef USE_SHERPAONNX
    if (impl_ && impl_->sherpa)
        return impl_->sherpa_name.empty() ? "sherpa-onnx" : impl_->sherpa_name;
#endif
#ifdef USE_ONNXRUNTIME
    if (impl_ && impl_->whisper) return "whisper";
#endif
    return "Mock";
}

void ASR::set_callback(ASRCallback callback) {
    if (impl_) {
        impl_->callback = std::move(callback);
    }
}

void ASR::set_cancel_token(std::shared_ptr<CancelToken> token) {
    cancel_token_ = std::move(token);
}

void ASR::reset() {
    if (impl_) {
        impl_->audio_buffer.clear();
    }
}

// ========== 工厂函数 ==========

std::unique_ptr<ASR> create_sense_voice_asr() {
    return std::make_unique<ASR>();
}

std::unique_ptr<ASR> create_whisper_asr() {
    return std::make_unique<ASR>();
}

}  // namespace voice_agent
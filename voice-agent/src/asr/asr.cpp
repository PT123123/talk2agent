// src/asr/asr.cpp
#include "asr.hpp"
#include "util/log.hpp"
#include <algorithm>
#include <cmath>

namespace voice_agent {

// ========== ASR 实现（pimpl） ==========
struct ASR::Impl {
    ASRCallback callback;
    std::vector<float> audio_buffer;
    size_t samples_per_chunk = 0;

    Impl() = default;

    bool load_model(const ASRConfig& config) {
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

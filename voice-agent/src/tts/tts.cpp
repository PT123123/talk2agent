// src/tts/tts.cpp
#include "tts.hpp"
#include "util/log.hpp"
#include <random>
#include <cmath>
#include <thread>

namespace voice_agent {

// ========== TTS 实现（pimpl） ==========
struct TTS::Impl {
    TTSCallback callback;

    bool load(const TTSConfig& config) {
        LOG_INFO("MockTTS: initialized ({})", config.model_path);
        return true;
    }

    std::vector<int16_t> synthesize(const std::string& text) {
        // Generate 1 second of simple sine wave @ 24kHz
        std::vector<int16_t> audio(24000);
        for (size_t i = 0; i < audio.size(); ++i) {
            float t = static_cast<float>(i) / 24000.0f;
            audio[i] = static_cast<int16_t>(16000 * std::sin(2.0f * 3.14159f * 440.0f * t));  // 440 Hz
        }
        LOG_INFO("MockTTS: synthesized {} chars -> {} samples", text.size(), audio.size());
        return audio;
    }

    void stop() {}
};

// ========== TTS 主类实现 ==========

TTS::TTS() = default;
TTS::~TTS() = default;

bool TTS::initialize(const TTSConfig& config) {
    config_ = config;
    impl_ = std::make_unique<Impl>();

    if (!impl_->load(config)) {
        LOG_ERROR("TTS: failed to load model");
        return false;
    }

    LOG_INFO("TTS: initialized {} Hz, speed={}, lang={}",
             config.sample_rate, config.speed, config.lang);
    return true;
}

std::vector<int16_t> TTS::synthesize(const std::string& text) {
    if (!impl_) return {};

    synthesizing_ = true;
    auto result = impl_->synthesize(text);
    synthesizing_ = false;
    return result;
}

void TTS::synthesize_stream(const std::string& text, TTSCallback callback) {
    if (!impl_) return;

    synthesizing_ = true;
    impl_->callback = callback;

    auto audio = impl_->synthesize(text);
    size_t chunk_size = 4800;  // 200ms @ 24kHz

    for (size_t i = 0; i < audio.size(); i += chunk_size) {
        size_t end = std::min(i + chunk_size, audio.size());
        bool is_last = (end >= audio.size());

        callback(audio.data() + i, end - i, is_last);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    synthesizing_ = false;
}

void TTS::stop() {
    synthesizing_ = false;
    if (impl_) {
        impl_->stop();
    }
}

void TTS::set_cancel_token(std::shared_ptr<CancelToken> token) {
    cancel_token_ = std::move(token);
}

// ========== 工厂函数 ==========

std::unique_ptr<TTS> create_kokoro_tts() {
    return std::make_unique<TTS>();
}

}  // namespace voice_agent

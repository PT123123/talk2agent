// src/orchestrator/audio_router.cpp
#include "audio_router.hpp"
#include "util/log.hpp"
#include <algorithm>
#include <cmath>

namespace voice_agent {

AudioRouter::AudioRouter() = default;
AudioRouter::~AudioRouter() = default;

void AudioRouter::start_playback() {
    playing_ = true;
    tts_buffer_.clear();
    tts_read_pos_ = 0;
    fading_out_ = false;
    LOG_INFO("AudioRouter: start playback mode");
}

void AudioRouter::stop_playback() {
    playing_ = false;
    tts_buffer_.clear();
    tts_read_pos_ = 0;
    fading_out_ = false;
    LOG_INFO("AudioRouter: stop playback mode");
}

void AudioRouter::push_tts_frames(const int16_t* pcm, size_t frames) {
    if (!playing_) return;
    size_t old_size = tts_buffer_.size();
    tts_buffer_.resize(old_size + frames);
    std::memcpy(tts_buffer_.data() + old_size, pcm, frames * sizeof(int16_t));
}

bool AudioRouter::get_playback_frames(int16_t* buffer, size_t frames) {
    if (!playing_ || tts_buffer_.empty()) return false;

    size_t remaining = tts_buffer_.size() - tts_read_pos_;
    size_t to_read = std::min(frames, remaining);

    if (fading_out_) {
        // 淡出：应用线性衰减
        size_t fade_pos = tts_read_pos_ - fade_out_start_frame_;
        for (size_t i = 0; i < to_read; ++i) {
            float gain = 1.0f - static_cast<float>(fade_pos + i) / static_cast<float>(fade_out_frames_);
            gain = std::max(0.0f, gain);
            buffer[i] = static_cast<int16_t>(tts_buffer_[tts_read_pos_ + i] * gain);
        }
    } else {
        std::memcpy(buffer, tts_buffer_.data() + tts_read_pos_, to_read * sizeof(int16_t));
    }

    tts_read_pos_ += to_read;

    // 检查是否播放完毕
    if (tts_read_pos_ >= tts_buffer_.size()) {
        return false;  // 播放完毕
    }

    return true;
}

}  // namespace voice_agent

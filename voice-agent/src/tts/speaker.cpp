// src/tts/speaker.cpp
#include "tts/speaker.hpp"
#include "tts/tts.hpp"
#include "audio/audio_device.hpp"
#include "util/log.hpp"

#include <chrono>
#include <thread>

namespace voice_agent {

TTSSpeaker::TTSSpeaker(TTS* tts) : tts_(tts) {}

TTSSpeaker::~TTSSpeaker() { stop(); }

size_t TTSSpeaker::onPlayback_(int16_t* output, size_t frames) {
    if (!playing_.load()) {
        for (size_t i = 0; i < frames; ++i) output[i] = 0;
        return frames;
    }
    size_t pos = cursor_.load();
    size_t write = 0;
    while (write < frames && pos < samples_.size()) {
        output[write++] = samples_[pos++];
    }
    for (; write < frames; ++write) output[write] = 0;  // 结尾补静音
    cursor_.store(pos);
    return frames;
}

bool TTSSpeaker::play(const std::string& text) {
    if (!tts_ || text.empty()) return false;
    stop();

    try {
        samples_ = tts_->synthesize(text);
    } catch (const std::exception& e) {
        LOG_ERROR("TTSSpeaker synthesize failed: {}", e.what());
        return false;
    }
    if (samples_.empty()) {
        LOG_WARN("TTSSpeaker: no audio synthesized for text");
        return false;
    }

    AudioConfig cfg;
    cfg.sample_rate = tts_->config().sample_rate ? tts_->config().sample_rate : 24000;
    cfg.channels = 1;
    cfg.frames_per_buffer = 480;  // 20ms @24kHz

    out_ = std::make_shared<AudioDevice>();
    if (!out_->init_as_output(cfg)) {
        out_.reset();
        LOG_ERROR("TTSSpeaker: failed to open output device");
        return false;
    }
    out_->set_playback_callback(
        [this](int16_t* output, size_t frames) { return onPlayback_(output, frames); });

    cursor_.store(0);
    playing_.store(true);
    if (!out_->start()) {
        playing_.store(false);
        out_.reset();
        return false;
    }

    // 阻塞等待播放完成（含最后一段缓冲区排空），带超时保护
    const double dur = static_cast<double>(samples_.size()) / cfg.sample_rate;
    const auto timeout = std::chrono::milliseconds(
        static_cast<long long>((dur + 1.5) * 1000.0));
    const auto begin = std::chrono::steady_clock::now();
    while (cursor_.load() < samples_.size()) {
        if (std::chrono::steady_clock::now() - begin > timeout) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // 让最后一帧输出完
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    playing_.store(false);
    out_->stop();
    out_.reset();
    LOG_INFO("TTSSpeaker finished playing {} samples", samples_.size());
    return true;
}

void TTSSpeaker::stop() {
    playing_.store(false);
    if (out_) {
        out_->stop();
        out_.reset();
    }
}

}  // namespace voice_agent
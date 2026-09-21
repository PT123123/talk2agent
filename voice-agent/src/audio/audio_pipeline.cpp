// src/audio/audio_pipeline.cpp
#include "audio_pipeline.hpp"
#include "audio_device.hpp"
#include "util/log.hpp"
#include <cmath>

namespace voice_agent {

// ========== AudioPipeline 实现 ==========

struct AudioPipeline::Impl {
    AudioDevice input_device;
    AudioDevice output_device;
    AudioInputCallback input_callback;
    AudioPlaybackCallback playback_callback;
};

AudioPipeline::AudioPipeline() : impl_(std::make_unique<Impl>()) {}

AudioPipeline::~AudioPipeline() {
    stop();
}

bool AudioPipeline::initialize(const AudioConfig& config) {
    config_ = config;
    
    // 初始化输入设备
    if (!impl_->input_device.init_as_input(config)) {
        LOG_ERROR("Failed to initialize input device");
        return false;
    }
    
    // 设置输入回调
    impl_->input_device.set_input_callback([this](const int16_t* data, size_t frames) {
        // 计算输入电平（dBFS），供调试界面轮询
        if (data && frames > 0) {
            float sum = 0.0f;
            for (size_t i = 0; i < frames; ++i) {
                float s = static_cast<float>(data[i]) / 32768.0f;
                sum += s * s;
            }
            float rms = std::sqrt(sum / static_cast<float>(frames));
            last_input_level_db_.store(rms <= 1e-6f ? -96.0f
                                                    : 20.0f * std::log10(rms));
        }
        if (impl_->input_callback) {
            impl_->input_callback(data, frames);
        }
    });
    
    // 初始化输出设备
    if (!impl_->output_device.init_as_output(config)) {
        LOG_ERROR("Failed to initialize output device");
        return false;
    }
    
    LOG_INFO("Audio pipeline initialized");
    return true;
}

void AudioPipeline::set_input_callback(AudioInputCallback cb) {
    impl_->input_callback = std::move(cb);
}

void AudioPipeline::set_playback_callback(AudioPlaybackCallback cb) {
    impl_->playback_callback = std::move(cb);
}

bool AudioPipeline::start() {
    if (running_) return true;
    
    // 清空播放缓冲
    playback_buffer_.clear();
    
    // 启动播放线程
    playback_thread_ = std::thread([this]() {
        LOG_INFO("Playback thread started");
        while (running_) {
            int16_t buffer[1024];
            size_t frames = playback_buffer_.pop(buffer, 1024);
            if (frames > 0) {
                impl_->output_device.play(buffer, frames);
            } else {
                // 缓冲为空，填充静音
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        LOG_INFO("Playback thread stopped");
    });
    
    // 启动输入设备
    if (!impl_->input_device.start()) {
        LOG_ERROR("Failed to start input device");
        running_ = false;
        return false;
    }
    
    // 启动输出设备
    if (!impl_->output_device.start()) {
        LOG_ERROR("Failed to start output device");
        impl_->input_device.stop();
        running_ = false;
        return false;
    }
    
    running_ = true;
    LOG_INFO("Audio pipeline started");
    return true;
}

void AudioPipeline::stop() {
    if (!running_) return;
    
    running_ = false;
    
    impl_->input_device.stop();
    impl_->output_device.stop();
    
    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
    
    LOG_INFO("Audio pipeline stopped");
}

void AudioPipeline::on_input_data(const int16_t* data, size_t frames) {
    if (impl_->input_callback) {
        impl_->input_callback(data, frames);
    }
}

void AudioPipeline::on_playback_data(int16_t* data, size_t frames) {
    if (impl_->playback_callback) {
        impl_->playback_callback(data, frames);
    }
}

size_t AudioPipeline::get_playback_buffered_frames() const {
    return playback_buffer_.available();
}

bool AudioPipeline::is_device_ready() const {
    return impl_->input_device.is_running() && impl_->output_device.is_running();
}

// ========== FullDuplexPipeline 实现 ==========

struct FullDuplexPipeline::Impl {
    AudioDevice input_device;
    AudioDevice output_device;
    std::function<void(const int16_t*, size_t)> aec_callback;
};

FullDuplexPipeline::FullDuplexPipeline() : impl_(std::make_unique<Impl>()) {}

FullDuplexPipeline::~FullDuplexPipeline() {
    stop();
}

bool FullDuplexPipeline::initialize(const AudioConfig& config) {
    config_ = config;
    LOG_INFO("Full duplex pipeline initialized (placeholder)");
    return true;
}

void FullDuplexPipeline::set_aec_callback(
    std::function<void(const int16_t*, size_t)> cb) {
    impl_->aec_callback = std::move(cb);
}

void FullDuplexPipeline::set_playback_callback(AudioPlaybackCallback cb) {
    (void)cb;
}

bool FullDuplexPipeline::start() {
    if (running_) return true;
    running_ = true;
    LOG_INFO("Full duplex pipeline started (placeholder)");
    return true;
}

void FullDuplexPipeline::stop() {
    if (!running_) return;
    running_ = false;
    LOG_INFO("Full duplex pipeline stopped");
}

size_t FullDuplexPipeline::write_playback(const int16_t* data, size_t frames) {
    (void)data;
    (void)frames;
    return frames;
}

}  // namespace voice_agent

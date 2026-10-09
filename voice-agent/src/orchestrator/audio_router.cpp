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
    fade_out_start_frame_ = 0;
    fade_out_frames_ = 0;
    output_gain_ = 1.0f;
    // 新一轮从零开始计播放进度
    total_frames_pushed_ = 0;
    total_frames_played_ = 0;
    LOG_INFO("AudioRouter: start playback mode");
}

void AudioRouter::stop_playback() {
    playing_ = false;
    tts_buffer_.clear();
    tts_read_pos_ = 0;
    fading_out_ = false;
    fade_out_start_frame_ = 0;
    fade_out_frames_ = 0;
    output_gain_ = 1.0f;
    LOG_INFO("AudioRouter: stop playback mode");
}

void AudioRouter::request_fade_out_stop() {
    if (!playing_ || tts_buffer_.size() <= tts_read_pos_) {
        // 没有可淡出的尾巴（没在播，或缓冲已放空）—— 退化为硬切
        stop_playback();
        return;
    }

    // 只保留"已缓冲但还没播出去"的尾巴里、fade_out_ms 覆盖得到的那一段：
    //   - tts_read_pos_ 之前的是用户已经听到的，既撤不回来也不该再放一遍
    //   - 淡出窗口之后的更不该放（Agent 已经被打断了，剩下的内容直接作废）
    // 两者都不留，缓冲恰好缩到 fade_out_frames_，播放回调拉完即止。
    const size_t remaining = tts_buffer_.size() - tts_read_pos_;
    size_t fade_frames = static_cast<size_t>(
        static_cast<long long>(std::max(1, fade_out_ms_)) * output_rate_ / 1000);
    if (fade_frames > remaining) fade_frames = remaining;
    if (fade_frames == 0) fade_frames = 1;

    tts_buffer_.erase(tts_buffer_.begin(),
                      tts_buffer_.begin() + static_cast<std::ptrdiff_t>(tts_read_pos_));
    tts_buffer_.resize(fade_frames);
    tts_read_pos_ = 0;

    fade_out_start_frame_ = 0;
    fade_out_frames_ = fade_frames;
    fading_out_ = true;
    output_gain_ = 1.0f;
    // playing_ 保持 true：播放回调要继续把这段尾巴拉出来淡到 0，
    // 放完后 get_playback_frames() 返回 false，由播放回调统一收尾。
    LOG_INFO("AudioRouter: fade out {} frames ({} ms @ {} Hz)",
             fade_frames, fade_out_ms_, output_rate_);
}

void AudioRouter::push_tts_frames(const int16_t* pcm, size_t frames) {
    if (!playing_) return;
    // 淡出中一律丢弃新帧：否则会"边淡边补"，听起来像 Agent 又接着说了。
    // 调用方仍应先 tts_->stop() 停合成，这里只是兜底。
    if (fading_out_) return;
    size_t old_size = tts_buffer_.size();
    tts_buffer_.resize(old_size + frames);
    std::memcpy(tts_buffer_.data() + old_size, pcm, frames * sizeof(int16_t));
    // 累计"已推给播放端"的量 —— 注意这不等于用户已听到（见 total_frames_played_）
    total_frames_pushed_ += static_cast<int64_t>(frames);
}

bool AudioRouter::get_playback_frames(int16_t* buffer, size_t frames) {
    if (!playing_ || tts_buffer_.size() <= tts_read_pos_) return false;

    size_t remaining = tts_buffer_.size() - tts_read_pos_;
    size_t to_read = std::min(frames, remaining);

    if (fading_out_) {
        // 线性衰减到 0。fade_out_frames_ 在 request_fade_out_stop() 里
        // 已按剩余量 clamp，最后一帧恰好为 0，不会截断出爆音。
        const size_t pos = tts_read_pos_;
        const float inv = 1.0f / static_cast<float>(fade_out_frames_);
        for (size_t i = 0; i < to_read; ++i) {
            float gain = 1.0f - static_cast<float>(pos + i) * inv;
            gain = std::clamp(gain, 0.0f, 1.0f);
            buffer[i] = static_cast<int16_t>(tts_buffer_[pos + i] * gain);
        }
    } else if (output_gain_ < 1.0f) {
        // duck：打断确认期间把 Agent 压低，别盖住用户说话
        for (size_t i = 0; i < to_read; ++i) {
            buffer[i] = static_cast<int16_t>(
                tts_buffer_[tts_read_pos_ + i] * output_gain_);
        }
    } else {
        std::memcpy(buffer, tts_buffer_.data() + tts_read_pos_,
                    to_read * sizeof(int16_t));
    }

    tts_read_pos_ += to_read;
    // 关键：这里才是"用户实际听到"的量。打断时用它算已播出部分。
    total_frames_played_ += static_cast<int64_t>(to_read);

    // 缓冲放完 → 返回 false，让播放回调收尾（自然播完 / 淡出结束）
    if (tts_read_pos_ >= tts_buffer_.size()) {
        return false;
    }

    return true;
}

double AudioRouter::played_ratio() const {
    if (total_frames_pushed_ <= 0) return 0.0;
    const double r = static_cast<double>(total_frames_played_) /
                     static_cast<double>(total_frames_pushed_);
    return r < 0.0 ? 0.0 : (r > 1.0 ? 1.0 : r);
}

void AudioRouter::reset_progress() {
    total_frames_pushed_ = 0;
    total_frames_played_ = 0;
}

}  // namespace voice_agent
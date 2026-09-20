// src/orchestrator/audio_router.hpp
#pragma once
#include <functional>
#include <memory>
#include <vector>
#include "core/types.hpp"

namespace voice_agent {

// ========== Audio Router 音频路由 ==========
// 管理 Agent 说话时如何处理音频流：
//   - 播放 TTS 音频时，混音 + 淡出用户输入
//   - 打断检测：监听用户输入，检测打断意图

class AudioRouter {
public:
    using PcmCallback = std::function<void(const int16_t* pcm, size_t frames)>;

    AudioRouter();
    ~AudioRouter();

    // 不可复制
    AudioRouter(const AudioRouter&) = delete;
    AudioRouter& operator=(const AudioRouter&) = delete;

    // 开始播放 TTS 音频（切换到播放模式）
    void start_playback();

    // 停止播放，回到采集模式
    void stop_playback();

    // 推送 TTS 音频帧（会被混合后输出）
    void push_tts_frames(const int16_t* pcm, size_t frames);

    // 设置淡出回调（TTS 被用户打断时调用）
    void set_fade_out_callback(std::function<void()> cb) {
        fade_out_callback_ = std::move(cb);
    }

    // 获取混合后的播放音频
    bool get_playback_frames(int16_t* buffer, size_t frames);

    // 当前是否为播放模式
    bool is_playing() const { return playing_; }

    // 淡出参数
    void set_fade_out_ms(int ms) { fade_out_ms_ = ms; }
    void set_fade_in_ms(int ms) { fade_in_ms_ = ms; }

private:
    // 计算淡入淡出增益
    float compute_gain_(size_t frame_idx, size_t total_frames) const;

    bool playing_ = false;
    int fade_out_ms_ = 40;
    int fade_in_ms_ = 10;

    std::vector<int16_t> tts_buffer_;
    size_t tts_read_pos_ = 0;
    std::function<void()> fade_out_callback_;

    // 淡出状态
    bool fading_out_ = false;
    size_t fade_out_start_frame_ = 0;
    size_t fade_out_frames_ = 0;
};

}  // namespace voice_agent

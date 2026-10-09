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

    // 停止播放，回到采集模式（硬切：立刻丢弃缓冲与待播音频）
    void stop_playback();

    // 打断用：不是立刻硬切，而是把**当前缓冲的最后一段**在 fade_out_ms 内
    // 线性淡到 0 再停。
    //
    // 为什么需要：打断瞬间已经写进声卡缓冲的少量音频撤不回来，直接
    // tts_buffer_.clear() 会把波形从当前幅度截到 0，听感上是一声"咔"。
    // 40ms 的淡出足以把这段尾巴抹平，同时不至于让用户觉得 Agent 还在讲。
    //
    // 语义要点：
    //   - 只对"已经在缓冲里的音频"淡出，**不再接受新的 TTS 帧**
    //     （调用方需先停 TTS 合成，否则会边淡边补）
    //   - 淡出期间 playing_ 仍为 true，播放回调继续拉帧
    //   - 淡出走完后 get_playback_frames() 返回 false，由播放回调收尾
    //   - 缓冲已空或未在播放时退化为硬切
    void request_fade_out_stop();

    // 推送 TTS 音频帧（会被混合后输出）
    void push_tts_frames(const int16_t* pcm, size_t frames);

    // 播放输出采样率：用于把 fade_out_ms 换算成帧数。须与实际播放端一致。
    void set_output_rate(int hz) { output_rate_ = hz > 0 ? hz : 24000; }

    // 输出增益（1.0 = 原音量）。用于打断确认期间把 Agent 音量压低，
    // 让用户说话时不被 Agent 盖住 —— duck 与"是否打断"是两个独立控制面。
    void set_output_gain(float g) { output_gain_ = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g); }
    float output_gain() const { return output_gain_; }

    // 获取混合后的播放音频
    bool get_playback_frames(int16_t* buffer, size_t frames);

    // 当前是否为播放模式
    bool is_playing() const { return playing_; }

    // 淡出参数
    void set_fade_out_ms(int ms) { fade_out_ms_ = ms; }
    void set_fade_in_ms(int ms) { fade_in_ms_ = ms; }

    // ---- 播放进度（打断时用来算"用户实际听到多少"）----
    // 业界共识（OpenAI conversation.item.truncate / Azure auto_truncate）：
    // 打断后必须让模型知道用户实际听到哪一段，否则它会以为没播的部分也说过，
    // 下一轮基于错误前提推理 —— 这就是 context desync。
    //
    // 已推送的采样数（= 模型生成的总量）
    int64_t total_frames_pushed() const { return total_frames_pushed_; }
    // 已播放的采样数（= 用户实际听到的量）
    int64_t total_frames_played() const { return total_frames_played_; }
    // 播放比例 0~1。已推送但未播的部分就是被截断的内容。
    double played_ratio() const;

    void reset_progress();

private:
    bool playing_ = false;
    int fade_out_ms_ = 40;
    int fade_in_ms_ = 10;
    int output_rate_ = 24000;     // 播放输出采样率（fade_out_ms 换算成帧用）
    float output_gain_ = 1.0f;    // 打断确认期间的 duck 音量

    std::vector<int16_t> tts_buffer_;
    size_t tts_read_pos_ = 0;

    // 播放进度统计（采样数）
    int64_t total_frames_pushed_{0};
    int64_t total_frames_played_{0};

    // 淡出状态：fading_out_ 为真时 push_tts_frames() 一律丢弃
    bool fading_out_ = false;
    size_t fade_out_start_frame_ = 0;
    size_t fade_out_frames_ = 0;
};

}  // namespace voice_agent

// src/audio/audio_pipeline.hpp
#pragma once
#include "audio_device.hpp"
#include "core/ring_buffer.hpp"
#include "core/cancel_token.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <functional>

namespace voice_agent {

// 音频输入回调（pipeline → 调用者，只读）
using AudioInputCallback = std::function<void(const int16_t* pcm_data, size_t frames)>;
// 音频播放回调（调用者 → pipeline，填充输出缓冲区）
using AudioPlaybackCallback = std::function<void(int16_t* pcm_data, size_t frames)>;

class AudioPipeline {
public:
    AudioPipeline();
    ~AudioPipeline();

    // 初始化音频管道
    bool initialize(const AudioConfig& config);

    // 设置麦克风输入回调（VAD 会用到）
    void set_input_callback(AudioInputCallback cb);

    // 设置播放回调（接收要播放的音频）
    void set_playback_callback(AudioPlaybackCallback cb);

    // 启动管道
    bool start();

    // 停止管道
    void stop();

    // 获取音频配置
    const AudioConfig& config() const { return config_; }

    // 是否正在运行
    bool is_running() const { return running_.load(); }

    // 获取当前播放缓冲中的帧数
    size_t get_playback_buffered_frames() const;

    // 检查音频设备是否就绪
    bool is_device_ready() const;

private:
    // 内部数据处理
    void on_input_data(const int16_t* data, size_t frames);
    void on_playback_data(int16_t* data, size_t frames);

    // 播放线程（从缓冲读取并播放）
    void playback_thread_func();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    AudioConfig config_;
    std::atomic<bool> running_{false};
    
    // 播放缓冲（接收 TTS 音频）- 必须是 2 的幂次
    static constexpr size_t PLAYBACK_BUFFER_SIZE = 262144;  // ~5.5秒缓冲 @ 48kHz
    SpscRingBuffer<int16_t, PLAYBACK_BUFFER_SIZE> playback_buffer_;
    
    std::thread playback_thread_;
};

// 简化的全双工音频管道（输入+输出同步）
class FullDuplexPipeline {
public:
    FullDuplexPipeline();
    ~FullDuplexPipeline();

    // 初始化
    bool initialize(const AudioConfig& config);

    // 设置 AEC 处理回调
    void set_aec_callback(std::function<void(const int16_t* echo_cancelled, size_t frames)> cb);

    // 设置回放数据回调（接收要播放的音频）
    void set_playback_callback(AudioPlaybackCallback cb);

    // 启动
    bool start();

    // 停止
    void stop();

    // 写入要播放的音频
    size_t write_playback(const int16_t* data, size_t frames);

    bool is_running() const { return running_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    AudioConfig config_;
    std::atomic<bool> running_{false};
};

}  // namespace voice_agent

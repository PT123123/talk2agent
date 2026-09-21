// src/tts/speaker.hpp
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace voice_agent {

class TTS;
class AudioDevice;

// ========== TTS 播报 ==========
// 把一段文本经 TTS 合成后，通过音频输出设备实时播放。play() 阻塞直到播完。
class TTSSpeaker {
public:
    explicit TTSSpeaker(TTS* tts);
    ~TTSSpeaker();

    // 合成并播放（阻塞）。text 为空或合成失败返回 false。
    bool play(const std::string& text);

    // 中断当前播放
    void stop();

private:
    TTS* tts_;
    std::shared_ptr<AudioDevice> out_;   // 播放设备
    std::vector<int16_t> samples_;       // 待播采样（含遍历游标在回调中推进）
    std::atomic<size_t> cursor_{0};
    std::atomic<bool> playing_{false};

    // 播放回调：从 samples_ 取出 frame_count 个采样填入输出
    size_t onPlayback_(int16_t* output, size_t frames);
};

}  // namespace voice_agent
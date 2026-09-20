// src/audio/echo_cancellation.hpp
#pragma once
#include <cstddef>
#include <memory>
#include <functional>

namespace voice_agent {

// AEC 配置
struct AECConfig {
    int sample_rate = 48000;
    int frame_size = 480;           // 10ms @ 48kHz
    int filter_length_ms = 200;     // 滤波器长度（毫秒）
    int noise_suppression_level = 0;  // 0=关闭, 1=-6dB, 2=-12dB, 3=-18dB
    bool enable_agc = false;        // 自动增益控制
    bool enable_vad = false;        // 语音活动检测
};

// AEC 统计信息
struct AECStats {
    double erle_db = 0;             // 回声衰减（越高越好）
    double erl_db = 0;              // 回声返回损失
    double tail_length_ms = 0;      // 估计的尾长
    bool converged = false;         // 是否收敛
};

// 简化的 AEC 接口
// 注意：这是一个接口定义，实际的 AEC 算法需要集成 speexdsp 或其他库
class EchoCanceller {
public:
    EchoCanceller();
    ~EchoCanceller();

    // 初始化 AEC
    bool initialize(const AECConfig& config);

    // 处理一帧音频（输入 + 参考信号 → 消除后的输出）
    // input: 麦克风输入（含回声）
    // reference: 播放参考信号（扬声器输出）
    // output: AEC 处理后的输出（回声消除）
    // 返回实际处理的样本数
    size_t process(const int16_t* input, const int16_t* reference, int16_t* output);

    // 处理一帧音频（仅输入，用于不需要 AEC 的情况）
    size_t process(const int16_t* input, int16_t* output);

    // 获取统计信息
    AECStats get_stats() const;

    // 重置 AEC 状态
    void reset();

    // 是否初始化
    bool is_initialized() const { return initialized_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AECConfig config_;
    bool initialized_ = false;
};

// 辅助函数：检测音频是否包含语音
bool detect_voice_activity(const int16_t* pcm, size_t frames, int sample_rate);

// 辅助函数：计算音频能量（dB）
double calculate_energy_db(const int16_t* pcm, size_t frames);

}  // namespace voice_agent

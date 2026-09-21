// src/audio/echo_cancellation.cpp
// 回声消除（AEC）实现
// 使用简化的自适应滤波器算法
// 后续可以集成 speexdsp 的 MDF 算法

#include "echo_cancellation.hpp"
#include "util/log.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

namespace voice_agent {

// ========== 简化 AEC 实现 ==========
// 注意：这是一个参考实现，实际产品应该使用 speexdsp 或 WebRTC AEC

struct EchoCanceller::Impl {
    // 简化的 FIR 滤波器系数
    std::vector<float> filter_coeffs;
    std::vector<float> reference_buffer;  // 参考信号缓冲
    
    size_t filter_taps;
    size_t buffer_pos = 0;
    float adaptation_speed = 0.01f;  // 适配速度
    float convergence_threshold = 0.1f;
    
    // 统计
    double total_echo_power = 0;
    double total_residual_power = 0;
    size_t frame_count = 0;
    
    Impl(size_t taps) : filter_taps(taps) {
        filter_coeffs.resize(filter_taps, 0.0f);
        reference_buffer.resize(filter_taps, 0.0f);
    }
};

EchoCanceller::EchoCanceller() : impl_(nullptr) {}

EchoCanceller::~EchoCanceller() = default;

bool EchoCanceller::initialize(const AECConfig& config) {
    config_ = config;
    
    // 计算滤波器抽头数
    size_t filter_taps = (config.sample_rate * config.filter_length_ms) / 1000;
    // 确保是 2 的幂次（方便 FFT 优化）
    size_t power = 1;
    while (power < filter_taps) power *= 2;
    filter_taps = power;
    
    impl_ = std::make_unique<Impl>(filter_taps);
    initialized_ = true;
    
    LOG_INFO("AEC initialized: {} Hz, {} ms tail, {} taps",
             config.sample_rate, config.filter_length_ms, filter_taps);
    return true;
}

size_t EchoCanceller::process(const int16_t* input, const int16_t* reference, int16_t* output) {
    if (!initialized_ || !impl_) {
        // 没有初始化，直接复制
        std::memcpy(output, input, config_.frame_size * sizeof(int16_t));
        return config_.frame_size;
    }
    
    const size_t frames = config_.frame_size;
    auto& impl = *impl_;
    
    // 1. 将参考信号加入缓冲
    for (size_t i = 0; i < frames; i++) {
        impl.reference_buffer[impl.buffer_pos] = reference[i] / 32768.0f;
        impl.buffer_pos = (impl.buffer_pos + 1) % impl.filter_taps;
    }
    
    // 2. 简化的 NLMS 自适应滤波
    // y(n) = sum(h(k) * x(n-k))
    float echo_estimate = 0;
    for (size_t i = 0; i < impl.filter_taps; i++) {
        size_t idx = (impl.buffer_pos + i) % impl.filter_taps;
        echo_estimate += impl.filter_coeffs[i] * impl.reference_buffer[idx];
    }
    
    // 3. 计算误差 e(n) = d(n) - y(n)
    float error = 0;
    float input_power = 0;
    for (size_t i = 0; i < frames; i++) {
        float input_sample = input[i] / 32768.0f;
        input_power += input_sample * input_sample;
        
        // 误差 = 麦克风输入 - 回声估计
        error = input_sample - echo_estimate;
        
        // 4. 更新滤波器系数（NLMS 算法简化版）
        float reference_power = 0.001f;  // 防止除零
        for (size_t k = 0; k < impl.filter_taps; k++) {
            size_t idx = (impl.buffer_pos + k) % impl.filter_taps;
            reference_power += impl.reference_buffer[idx] * impl.reference_buffer[idx];
        }
        
        float mu = impl.adaptation_speed / reference_power;
        for (size_t k = 0; k < impl.filter_taps; k++) {
            size_t idx = (impl.buffer_pos + k) % impl.filter_taps;
            impl.filter_coeffs[k] += mu * error * impl.reference_buffer[idx];
        }
        
        // 5. 输出处理后的样本
        output[i] = static_cast<int16_t>(std::clamp(error * 32768.0f, -32768.0f, 32767.0f));
    }
    
    // 6. 更新统计
    impl.frame_count++;
    
    return frames;
}

size_t EchoCanceller::process(const int16_t* input, int16_t* output) {
    // 无参考信号，直接复制
    std::memcpy(output, input, config_.frame_size * sizeof(int16_t));
    return config_.frame_size;
}

AECStats EchoCanceller::get_stats() const {
    AECStats stats;
    if (!impl_) return stats;
    
    // 简化统计计算
    stats.erle_db = 10.0 * std::log10(impl_->total_echo_power / (impl_->total_residual_power + 1e-10));
    stats.tail_length_ms = static_cast<double>(impl_->filter_taps) * 1000.0 / config_.sample_rate;
    stats.converged = impl_->frame_count > 100;  // 简单判断收敛
    
    return stats;
}

void EchoCanceller::reset() {
    if (impl_) {
        std::fill(impl_->filter_coeffs.begin(), impl_->filter_coeffs.end(), 0.0f);
        impl_->buffer_pos = 0;
        impl_->frame_count = 0;
        impl_->total_echo_power = 0;
        impl_->total_residual_power = 0;
    }
}

// ========== 辅助函数实现 ==========

bool detect_voice_activity(const int16_t* pcm, size_t frames, int sample_rate) {
    // 简化的 VAD：基于能量阈值
    double energy = 0;
    for (size_t i = 0; i < frames; i++) {
        float sample = pcm[i] / 32768.0f;
        energy += sample * sample;
    }
    energy = std::sqrt(energy / frames);
    
    // 能量阈值（可调）
    double threshold = 0.01;
    return energy > threshold;
}

double calculate_energy_db(const int16_t* pcm, size_t frames) {
    double energy = 0;
    for (size_t i = 0; i < frames; i++) {
        float sample = pcm[i] / 32768.0f;
        energy += sample * sample;
    }
    
    if (energy < 1e-10) return -100.0;  // 几乎无声
    
    double db = 10.0 * std::log10(energy / frames);
    return std::max(db, -100.0);
}

}  // namespace voice_agent

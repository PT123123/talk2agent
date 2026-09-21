// src/vad/vad.hpp
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <atomic>

namespace voice_agent {

// VAD 事件类型
enum class VADEvent {
    SpeechStart,      // 开始检测到语音
    SpeechEnd,        // 语音结束
    SpeechOngoing,    // 语音进行中
    Silence,         // 静音
};

// VAD 配置
struct VADConfig {
    int sample_rate = 16000;           // 采样率（ten-vad 需要 16kHz）
    int frame_length_ms = 32;          // 帧长（毫秒）
    int min_speech_duration_ms = 250;  // 最小语音持续时间
    int min_silence_duration_ms = 500; // 最小静音持续时间（触发 speech end）
    float speech_threshold = 0.5f;    // 语音检测阈值
    bool enable_silero_refine = true;  // 启用 silero 复核
    bool use_gpu = true;           // 优先用 DirectML GPU（运行时支持时），否则 CPU
};

// VAD 回调
using VADCallback = std::function<void(VADEvent event, const int16_t* pcm_data, size_t frames)>;

/**
 * VAD (Voice Activity Detection) 接口
 * 支持 ten-vad 主检测 + silero 复核
 */
class VAD {
public:
    VAD();
    ~VAD();

    // 初始化 VAD（加载模型）
    bool initialize(const VADConfig& config);
    
    // 手动设置模型路径（可选）
    void set_model_path(const std::string& ten_vad_path, const std::string& silero_path = "");

    // 处理音频数据（16kHz, 16-bit PCM）
    // 返回: 是否检测到语音
    bool process(const int16_t* pcm_data, size_t frames);

    // 重置状态
    void reset();

    // 设置回调
    void set_callback(VADCallback callback);

    // 获取当前状态
    bool is_speaking() const { return is_speaking_; }
    float get_speech_probability() const { return speech_prob_; }

    // 生效推理后端标签："DirectML GPU" / "CPU"
    std::string provider_label() const;

    // 最近一次单帧推理耗时（毫秒，供调试面板轮询）
    std::atomic<double> last_inference_ms{0.0};

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VADConfig config_;
    std::atomic<bool> is_speaking_{false};
    std::atomic<float> speech_prob_{0.0f};
};

/**
 * 打断探测器 - 5 层判定机制
 * 
 * 层级:
 * 1. VAD 语音开始
 * 2. 能量突变检测
 * 3. 频谱特征变化
 * 4. 语义模型（可选）
 * 5. 最终确认
 */
class InterruptionDetector {
public:
    struct InterruptionConfig {
        float energy_jump_threshold = 3.0f;      // 能量突变倍数
        int energy_history_size = 10;            // 能量历史窗口
        float speech_continue_threshold = 0.3f;  // 继续说话阈值
        int min_interruption_frames = 3;         // 最小打断帧数
    };

    InterruptionDetector();
    ~InterruptionDetector();

    // 初始化
    bool initialize(const InterruptionConfig& config);

    // 处理一帧音频数据
    // 返回: 是否检测到打断
    bool process(const int16_t* pcm_data, size_t frames);

    // 重置状态
    void reset();

    // 设置打断回调
    void set_callback(std::function<void()> callback);

    // 获取打断置信度
    float get_confidence() const { return confidence_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    InterruptionConfig config_;
    std::atomic<float> confidence_{0.0f};
    std::function<void()> interruption_callback_;
};

// 工具函数
namespace vad_utils {
    // 计算音频能量 (RMS)
    float calculate_rms(const int16_t* pcm, size_t frames);
    
    // 计算分贝 (dB)
    float to_db(float rms);
    
    // 静音检测
    bool is_silence(const int16_t* pcm, size_t frames, float threshold_db = -40.0f);
    
    // 重采样 48kHz -> 16kHz
    void downsample_48k_to_16k(const int16_t* input, size_t input_frames,
                                int16_t* output, size_t output_capacity);
}

}  // namespace voice_agent

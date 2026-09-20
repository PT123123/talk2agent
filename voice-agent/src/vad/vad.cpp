// src/vad/vad.cpp
#include "vad.hpp"
#include "util/log.hpp"
#include <algorithm>
#include <cmath>
#include <deque>

#ifdef USE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace voice_agent {

// ========== 工具函数实现 ==========

namespace vad_utils {

float calculate_rms(const int16_t* pcm, size_t frames) {
    if (frames == 0 || pcm == nullptr) return 0.0f;
    
    float sum = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        float sample = static_cast<float>(pcm[i]) / 32768.0f;
        sum += sample * sample;
    }
    return std::sqrt(sum / static_cast<float>(frames));
}

float to_db(float rms) {
    if (rms <= 0.0f) return -96.0f;  // 最小 dB
    return 20.0f * std::log10(rms);
}

bool is_silence(const int16_t* pcm, size_t frames, float threshold_db) {
    float rms = calculate_rms(pcm, frames);
    float db = to_db(rms);
    return db < threshold_db;
}

void downsample_48k_to_16k(const int16_t* input, size_t input_frames,
                           int16_t* output, size_t output_capacity) {
    // 48kHz / 16kHz = 3:1 下采样
    size_t output_frames = input_frames / 3;
    if (output_frames > output_capacity) {
        output_frames = output_capacity;
    }
    
    for (size_t i = 0; i < output_frames; i++) {
        // 简单的平均池化下采样
        int32_t sum = 0;
        for (int j = 0; j < 3; j++) {
            sum += input[i * 3 + j];
        }
        output[i] = static_cast<int16_t>(sum / 3);
    }
}

}  // namespace vad_utils

// ========== VAD 实现 ==========

struct VAD::Impl {
    VADConfig config;
    VADCallback callback;
    
#ifdef USE_ONNXRUNTIME
    std::unique_ptr<Ort::Env> ort_env;
    std::unique_ptr<Ort::Session> ten_vad_session;
    std::unique_ptr<Ort::Session> silero_session;
    std::vector<float> input_buffer;  // 16kHz PCM 缓冲
    std::vector<float> input_tensor;
    std::vector<float> output_tensor;
#endif
    
    // 状态
    bool vad_active = false;
    int silence_frames = 0;
    int speech_frames = 0;
    int min_speech_frames = 0;
    int min_silence_frames = 0;
    
    Impl() {
#ifdef USE_ONNXRUNTIME
        ort_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING);
#endif
    }
    
    bool load_ten_vad(const std::string& model_path) {
#ifdef USE_ONNXRUNTIME
        if (model_path.empty()) {
            LOG_WARN("ten-vad model path not set, using energy-based fallback");
            return false;
        }
        
        try {
            Ort::SessionOptions session_options;
            session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
            
            ten_vad_session = std::make_unique<Ort::Session>(*ort_env, model_path.c_str(), session_options);
            LOG_INFO("ten-vad model loaded: {}", model_path);
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to load ten-vad model: {}", e.what());
            return false;
        }
#else
        LOG_WARN("ONNX Runtime not available, using energy-based VAD");
        (void)model_path;
        return false;
#endif
    }
    
    bool process_frame_energy(const int16_t* pcm, size_t frames) {
        float rms = vad_utils::calculate_rms(pcm, frames);
        float db = vad_utils::to_db(rms);
        
        // 能量阈值判断
        bool is_speech = db > -40.0f;  // 能量阈值
        
        if (is_speech) {
            speech_frames++;
            silence_frames = 0;
        } else {
            silence_frames++;
            speech_frames = 0;
        }
        
        // 更新置信度
        float prob = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 40.0f));
        
        // 检测语音开始
        if (!vad_active && speech_frames >= min_speech_frames) {
            vad_active = true;
            if (callback) {
                callback(VADEvent::SpeechStart, pcm, frames);
            }
        }
        
        // 检测语音结束
        if (vad_active && silence_frames >= min_silence_frames) {
            vad_active = false;
            if (callback) {
                callback(VADEvent::SpeechEnd, nullptr, 0);
            }
        }
        
        return vad_active;
    }
    
#ifdef USE_ONNXRUNTIME
    bool process_frame_onnx(const int16_t* pcm, size_t frames, bool& is_speech) {
        if (!ten_vad_session) {
            return process_frame_energy(pcm, frames);
        }
        
        // 转换为 float 并归一化
        input_tensor.resize(frames);
        for (size_t i = 0; i < frames; i++) {
            input_tensor[i] = static_cast<float>(pcm[i]) / 32768.0f;
        }
        
        // 准备输入
        int64_t input_shape[] = {1, static_cast<int64_t>(frames)};
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocation, OrtMemTypeDefault);
        
        auto input_tensor = Ort::Value::CreateTensor<float>(
            memory_info, input_tensor.data(), input_tensor.size(),
            input_shape, 2);
        
        const char* input_names[] = {"input"};
        const char* output_names[] = {"output"};
        
        auto output_tensors = ten_vad_session->Run(
            Ort::RunOptions{nullptr},
            input_names, &input_tensor, 1,
            output_names, 1);
        
        float* output_data = output_tensors[0].GetTensorMutableData<float>();
        
        // 获取语音概率
        float speech_prob = output_data[0];
        
        // 状态机逻辑
        if (speech_prob > config.speech_threshold) {
            speech_frames++;
            silence_frames = 0;
        } else {
            silence_frames++;
            speech_frames = 0;
        }
        
        is_speech = speech_prob > config.speech_threshold;
        
        // 检测语音开始
        if (!vad_active && speech_frames >= min_speech_frames) {
            vad_active = true;
            if (callback) {
                callback(VADEvent::SpeechStart, pcm, frames);
            }
        }
        
        // 检测语音结束
        if (vad_active && silence_frames >= min_silence_frames) {
            vad_active = false;
            if (callback) {
                callback(VADEvent::SpeechEnd, nullptr, 0);
            }
        }
        
        return vad_active;
    }
#endif
};

VAD::VAD() : impl_(std::make_unique<Impl>()) {}
VAD::~VAD() = default;

bool VAD::initialize(const VADConfig& config) {
    impl_->config = config;
    
    // 计算最小帧数
    impl_->min_speech_frames = config.min_speech_duration_ms / config.frame_length_ms;
    impl_->min_silence_frames = config.min_silence_duration_ms / config.frame_length_ms;
    
    LOG_INFO("VAD initialized: {} Hz, {} ms/frame, speech_thresh={}",
             config.sample_rate, config.frame_length_ms, config.speech_threshold);
    return true;
}

void VAD::set_model_path(const std::string& ten_vad_path, const std::string& silero_path) {
    impl_->load_ten_vad(ten_vad_path);
    // silero 路径暂未使用
    (void)silero_path;
}

bool VAD::process(const int16_t* pcm_data, size_t frames) {
    if (frames == 0) return false;
    
    // 确保是 16kHz
    if (impl_->config.sample_rate == 16000) {
#ifdef USE_ONNXRUNTIME
        bool is_speech = false;
        return impl_->process_frame_onnx(pcm_data, frames, is_speech);
#else
        return impl_->process_frame_energy(pcm_data, frames);
#endif
    } else {
        // 48kHz -> 16kHz 下采样
        std::vector<int16_t> pcm_16k(frames / 3);
        vad_utils::downsample_48k_to_16k(pcm_data, frames, pcm_16k.data(), pcm_16k.size());
        
#ifdef USE_ONNXRUNTIME
        bool is_speech = false;
        return impl_->process_frame_onnx(pcm_16k.data(), pcm_16k.size(), is_speech);
#else
        return impl_->process_frame_energy(pcm_16k.data(), pcm_16k.size());
#endif
    }
}

void VAD::reset() {
    impl_->vad_active = false;
    impl_->silence_frames = 0;
    impl_->speech_frames = 0;
    is_speaking_ = false;
    speech_prob_ = 0.0f;
}

void VAD::set_callback(VADCallback callback) {
    impl_->callback = std::move(callback);
}

// ========== 打断探测器实现 ==========

struct InterruptionDetector::Impl {
    InterruptionConfig config;
    std::function<void()> interruption_callback;
    
    std::deque<float> energy_history;
    std::deque<bool> speech_history;
    
    int interruption_count = 0;
    float confidence = 0.0f;
    
    bool was_speaking = false;
    
    bool detect_energy_jump(float current_energy) {
        if (energy_history.empty()) return false;
        
        float avg_energy = 0.0f;
        for (float e : energy_history) avg_energy += e;
        avg_energy /= energy_history.size();
        
        if (avg_energy < 1e-6f) return false;
        
        float ratio = current_energy / avg_energy;
        return ratio > config.energy_jump_threshold;
    }
    
    float calculate_confidence(float current_energy) {
        if (energy_history.empty()) return 0.0f;
        
        float avg_energy = 0.0f, max_energy = 0.0f;
        for (float e : energy_history) {
            avg_energy += e;
            max_energy = std::max(max_energy, e);
        }
        avg_energy /= energy_history.size();
        
        // 基于能量突变的置信度
        float jump_conf = 0.0f;
        if (avg_energy > 1e-6f) {
            float ratio = current_energy / avg_energy;
            jump_conf = std::min(1.0f, (ratio - 1.0f) / (config.energy_jump_threshold - 1.0f));
        }
        
        // 基于语音历史的置信度
        float speech_conf = 0.0f;
        int speaking_count = 0;
        for (bool s : speech_history) {
            if (s) speaking_count++;
        }
        if (!speech_history.empty()) {
            speech_conf = static_cast<float>(speaking_count) / speech_history.size();
        }
        
        // 加权平均
        return jump_conf * 0.6f + speech_conf * 0.4f;
    }
};

InterruptionDetector::InterruptionDetector() : impl_(std::make_unique<Impl>()) {}
InterruptionDetector::~InterruptionDetector() = default;

bool InterruptionDetector::initialize(const InterruptionConfig& config) {
    impl_->config = config;
    LOG_INFO("InterruptionDetector initialized: energy_jump={}x, min_frames={}",
             config.energy_jump_threshold, config.min_interruption_frames);
    return true;
}

bool InterruptionDetector::process(const int16_t* pcm_data, size_t frames) {
    if (frames == 0) return false;
    
    float rms = vad_utils::calculate_rms(pcm_data, frames);
    float db = vad_utils::to_db(rms);
    bool is_speech = db > -40.0f;
    
    // 更新历史
    impl_->energy_history.push_back(rms);
    impl_->speech_history.push_back(is_speech);
    
    if (impl_->energy_history.size() > static_cast<size_t>(impl_->config.energy_history_size)) {
        impl_->energy_history.pop_front();
        impl_->speech_history.pop_front();
    }
    
    // 检测打断
    bool is_interruption = false;
    
    if (impl_->was_speaking && is_speech) {
        // 之前在说话，现在继续说
        if (impl_->detect_energy_jump(rms)) {
            impl_->interruption_count++;
            impl_->confidence = impl_->calculate_confidence(rms);
            
            if (impl_->interruption_count >= impl_->config.min_interruption_frames) {
                is_interruption = true;
                if (impl_->interruption_callback) {
                    impl_->interruption_callback();
                }
                LOG_INFO("Interruption detected! confidence={:.2f}", impl_->confidence);
            }
        } else {
            impl_->interruption_count = 0;
        }
    }
    
    impl_->was_speaking = is_speech;
    impl_->confidence = impl_->calculate_confidence(rms);
    
    return is_interruption;
}

void InterruptionDetector::reset() {
    impl_->energy_history.clear();
    impl_->speech_history.clear();
    impl_->interruption_count = 0;
    impl_->confidence = 0.0f;
    impl_->was_speaking = false;
    confidence_ = 0.0f;
}

void InterruptionDetector::set_callback(std::function<void()> callback) {
    impl_->interruption_callback = std::move(callback);
}

}  // namespace voice_agent

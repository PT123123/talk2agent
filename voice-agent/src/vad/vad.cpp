// src/vad/vad.cpp
#include "vad.hpp"
#include "util/log.hpp"
#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>

#ifdef USE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

#include "util/gpu.hpp"

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

    // Silero 流式 VAD：输入波形 + 若干状态参数（LSTM h/c，或单 state）→ prob + 更新后的状态。
    // 具体输入名/形状在 load_silero 时探测，运行时按探测结果动态构建，适配 v4/v5 两种签名。
    bool silero_loaded = false;
    std::string silero_wav_name;                       // 波形输入名，如 "x" / "input"
    std::vector<std::string> silero_state_names;       // 状态输入名，如 {"h","c"} / {"state"}
    std::vector<std::vector<int64_t>> silero_state_shapes;  // 每个状态张量的形状
    std::vector<std::vector<float>> silero_states;     // 与 state_names 对齐，逐帧更新
    bool silero_has_sr = false;                        // 模型是否需要 sr 输入
    std::string silero_sr_name;
    std::vector<std::string> silero_out_names;         // 输出名（prob + new_states）
    std::vector<float> silero_pcm_buf;                 // 不足 kSileroChunk 的残差
    static constexpr int kSileroChunk = 512;           // 30ms @16k
#endif
    
    // 状态
    bool vad_active = false;
    int silence_frames = 0;
    int speech_frames = 0;
    int min_speech_frames = 0;
    int min_silence_frames = 0;

    // 生效推理后端（ten-vad / silero 加载时记录）
    std::string provider_ = "CPU";
    
    // 重置 Silero 状态（新语音段开始）
    void reset_silero() {
#ifdef USE_ONNXRUNTIME
        for (auto& s : silero_states) {
            std::fill(s.begin(), s.end(), 0.0f);
        }
        silero_pcm_buf.clear();
#endif
    }
    
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
            const bool dml = config.use_gpu && ort_try_append_dml(&session_options);
            provider_ = (config.use_gpu && dml) ? "DirectML GPU" : "CPU";
            
            ten_vad_session = std::make_unique<Ort::Session>(
                *ort_env, std::wstring(model_path.begin(), model_path.end()).c_str(),
                session_options);
            LOG_INFO("ten-vad model loaded: {} (provider={})", model_path, dml ? "dml" : "cpu");
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

    // Silero 流式 ONNX 加载：按输入签名动态适配
    //  - LSTM v5：x[1,512] + h[2,1,64] + c[2,1,64] → prob + new_h + new_c
    //  - 单状态 v5：input[1,512] + state[2,1,128] (+ sr[1]) → out + stateN
    // 失败（例如无状态输入的整段变体）返回 false 由外部回退能量检测
    bool load_silero(const std::string& model_path) {
#ifdef USE_ONNXRUNTIME
        if (model_path.empty() || !std::filesystem::exists(model_path)) {
            LOG_WARN("silero VAD model not found: '{}'", model_path);
            return false;
        }
        try {
            silero_state_names.clear();
            silero_state_shapes.clear();
            silero_states.clear();
            silero_out_names.clear();
            silero_wav_name.clear();
            silero_has_sr = false;

            Ort::SessionOptions so;
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
            const bool dml = config.use_gpu && ort_try_append_dml(&so);
            provider_ = (config.use_gpu && dml) ? "DirectML GPU" : "CPU";
            silero_session = std::make_unique<Ort::Session>(
                *ort_env, std::wstring(model_path.begin(), model_path.end()).c_str(), so);

            Ort::AllocatorWithDefaultOptions alloc;
            const size_t nin = silero_session->GetInputCount();
            for (size_t i = 0; i < nin; ++i) {
                auto n = silero_session->GetInputNameAllocated(i, alloc);
                std::string name(n.get());
                auto ti = silero_session->GetInputTypeInfo(i);
                auto sh = ti.GetTensorTypeAndShapeInfo().GetShape();
                if (name == "sr" || name == "sr_tensor") {
                    silero_has_sr = true;
                    silero_sr_name = name;
                    continue;
                }
                if (sh.size() >= 3) {  // 状态参数（h/c 或 state 均为 3 维）
                    int64_t flat = 1;
                    std::vector<int64_t> full;
                    for (size_t d = 0; d < sh.size(); ++d) {
                        int64_t v = sh[d] > 0 ? sh[d] : 1;
                        full.push_back(v);
                        flat *= v;
                    }
                    silero_state_names.push_back(name);
                    silero_state_shapes.push_back(full);
                    silero_states.emplace_back((size_t)flat, 0.0f);
                } else {  // 波形输入（2 维 [1,N]）
                    silero_wav_name = name;
                }
            }
            if (silero_wav_name.empty() || silero_state_names.empty()) {
                LOG_ERROR("silero: unsupported signature (need waveform + state inputs)");
                silero_loaded = false;
                return false;
            }

            // 捕获输出名（prob + 各状态更新），Run 时显式指定，避免 nullptr 触发歧义
            const size_t nout = silero_session->GetOutputCount();
            for (size_t i = 0; i < nout; ++i) {
                auto n = silero_session->GetOutputNameAllocated(i, alloc);
                silero_out_names.emplace_back(n.get());
            }

            std::string states;
            for (auto& s : silero_state_names) states += s + " ";
            silero_loaded = true;
            LOG_INFO("silero VAD loaded: {} (wav='{}' states=[{}] sr={} provider={})",
                     model_path, silero_wav_name, states, silero_has_sr, dml ? "dml" : "cpu");
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("silero VAD load failed: {}", e.what());
            silero_loaded = false;
            return false;
        }
#else
        (void)model_path;
        return false;
#endif
    }

    // 流式 run 一帧 Silero（512 样本 / 30ms），返回语音概率
    float run_silero_frame(const float* samples) {
#ifdef USE_ONNXRUNTIME
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<Ort::Value> in_v;
        std::vector<const char*> in_n;

        // 波形输入 [1,512]
        std::array<int64_t, 2> wav_shape{1, kSileroChunk};
        in_v.push_back(Ort::Value::CreateTensor<float>(
            mem, const_cast<float*>(samples), kSileroChunk, wav_shape.data(), wav_shape.size()));
        in_n.push_back(silero_wav_name.c_str());

        // 状态输入（h/c 各一个，或单 state）
        for (size_t k = 0; k < silero_state_names.size(); ++k) {
            auto& sh = silero_state_shapes[k];
            in_v.push_back(Ort::Value::CreateTensor<float>(
                mem, silero_states[k].data(), silero_states[k].size(),
                sh.data(), sh.size()));
            in_n.push_back(silero_state_names[k].c_str());
        }

        // 采样率输入（若模型需要）
        std::array<int64_t, 1> sr_shape{1};
        std::vector<int64_t> sr_vec{16000};
        if (silero_has_sr) {
            in_v.push_back(Ort::Value::CreateTensor<int64_t>(
                mem, sr_vec.data(), 1, sr_shape.data(), sr_shape.size()));
            in_n.push_back(silero_sr_name.c_str());
        }

        const size_t nout = silero_out_names.empty() ? silero_state_names.size() + 1
                                                     : silero_out_names.size();
        float prob = 0.0f;
        try {
            // 显式指定输出名：ORT 1.19 CPU EP 对无状态输入的正常多输出可用 nullptr，
            // 但此 LSTM 模型带状态更新，传 nullptr 在 1.19 会触发 fail-fast，必须显式命名。
            std::vector<const char*> out_n;
            for (auto& s : silero_out_names) out_n.push_back(s.c_str());
            auto outs = silero_session->Run(Ort::RunOptions{nullptr}, in_n.data(), in_v.data(),
                                            in_n.size(), out_n.data(), (int)nout);
            prob = outs[0].GetTensorData<float>()[0];

            // 回写更新后的状态（out[1..]）
            for (size_t k = 0; k < silero_state_names.size(); ++k) {
                auto info = outs[1 + k].GetTensorTypeAndShapeInfo();
                auto sh = info.GetShape();
                int64_t n = 1;
                for (auto d : sh) n *= d;
                if (n == (int64_t)silero_states[k].size()) {
                    const float* d = outs[1 + k].GetTensorData<float>();
                    std::copy(d, d + n, silero_states[k].begin());
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("silero run failed: {}", e.what());
        } catch (...) {}
        return prob;
#else
        (void)samples;
        return 0.0f;
#endif
    }

    // 处理一整块 16kHz 音频：攒 512 样本 → Silero 推理 → 状态机
    bool process_frame_silero(const int16_t* pcm, size_t frames, bool& is_speech) {
        if (!silero_loaded) {
            return process_frame_energy(pcm, frames);
        }
        is_speech = false;

        for (size_t i = 0; i < frames; ++i) {
            silero_pcm_buf.push_back(static_cast<float>(pcm[i]) / 32768.0f);
        }

        while (silero_pcm_buf.size() >= (size_t)kSileroChunk) {
            float prob = run_silero_frame(silero_pcm_buf.data());
            silero_pcm_buf.erase(silero_pcm_buf.begin(), silero_pcm_buf.begin() + kSileroChunk);

            if (prob > config.speech_threshold) {
                speech_frames++;
                silence_frames = 0;
            } else {
                silence_frames++;
                speech_frames = 0;
            }

            is_speech = prob > config.speech_threshold;

            if (!vad_active && speech_frames >= min_speech_frames) {
                vad_active = true;
                reset_silero();
                if (callback) callback(VADEvent::SpeechStart, pcm, frames);
            }
            if (vad_active && silence_frames >= min_silence_frames) {
                vad_active = false;
                reset_silero();
                if (callback) callback(VADEvent::SpeechEnd, nullptr, 0);
            }
        }
        return vad_active;
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
        
        // 转换整段为 float 并作为一个输入张量（ten-vad 单帧输入）
        std::vector<float> frame_float(frames);
        for (size_t i = 0; i < frames; ++i) {
            frame_float[i] = static_cast<float>(pcm[i]) / 32768.0f;
        }

        // 准备输入
        int64_t input_shape[] = {1, static_cast<int64_t>(frames)};
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        auto value_tensor = Ort::Value::CreateTensor<float>(
            memory_info, frame_float.data(), frame_float.size(),
            input_shape, 2);

        const char* input_names[] = {"input"};
        const char* output_names[] = {"output"};

        auto output_tensors = ten_vad_session->Run(
            Ort::RunOptions{nullptr},
            input_names, &value_tensor, 1,
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
    // Silero（主），加载失败回退 ten-vad/能量检测
    bool ok = impl_->load_silero(silero_path);
    if (ok) {
        // 30ms/帧（512@16k）重新换算最小帧数
        impl_->min_speech_frames = impl_->config.min_speech_duration_ms / 30;
        impl_->min_silence_frames = impl_->config.min_silence_duration_ms / 30;
        return;
    }
    impl_->load_ten_vad(ten_vad_path);
}

bool VAD::process(const int16_t* pcm_data, size_t frames) {
    if (frames == 0) return false;

    const auto t0 = std::chrono::steady_clock::now();
    bool out = false;
    // 确保是 16kHz
    if (impl_->config.sample_rate == 16000) {
#ifdef USE_ONNXRUNTIME
        bool is_speech = false;
        if (impl_->silero_loaded) {
            out = impl_->process_frame_silero(pcm_data, frames, is_speech);
        } else {
            out = impl_->process_frame_onnx(pcm_data, frames, is_speech);
        }
#else
        out = impl_->process_frame_energy(pcm_data, frames);
#endif
    } else {
        // 48kHz -> 16kHz 下采样
        std::vector<int16_t> pcm_16k(frames / 3);
        vad_utils::downsample_48k_to_16k(pcm_data, frames, pcm_16k.data(), pcm_16k.size());

#ifdef USE_ONNXRUNTIME
        bool is_speech = false;
        if (impl_->silero_loaded) {
            out = impl_->process_frame_silero(pcm_16k.data(), pcm_16k.size(), is_speech);
        } else {
            out = impl_->process_frame_onnx(pcm_16k.data(), pcm_16k.size(), is_speech);
        }
#else
        out = impl_->process_frame_energy(pcm_16k.data(), pcm_16k.size());
#endif
    }
    last_inference_ms.store(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count());
    return out;
}

void VAD::reset() {
    impl_->vad_active = false;
    impl_->silence_frames = 0;
    impl_->speech_frames = 0;
    impl_->reset_silero();
    is_speaking_ = false;
    speech_prob_ = 0.0f;
}

void VAD::set_callback(VADCallback callback) {
    impl_->callback = std::move(callback);
}

std::string VAD::provider_label() const {
    if (!impl_) return "Mock";
    return impl_->provider_;
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

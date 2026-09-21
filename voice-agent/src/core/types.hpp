// src/core/types.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <functional>

// ========== 音频类型 ==========
using Sample = int16_t;

struct AudioFrame {
    const Sample* data{nullptr};
    size_t frames{0};          // 样本数
    int sample_rate{16000};    // 采样率
    uint64_t hw_pts_us{0};     // 硬件时间戳（微秒）
};

using PcmSink = std::function<void(const AudioFrame&)>;

// ========== 事件类型 ==========
enum class EventType {
    // VAD 事件
    VadStart,
    VadEnd,
    // 轮次判定
    TurnComplete,
    TurnIncomplete,
    // 打断
    BargeIn,
    // ASR
    AsrPartial,
    AsrFinal,
    // LLM
    LlmToken,
    LlmComplete,
    LlmToolCall,
    // TTS
    TtsStart,
    TtsChunk,
    TtsEnd,
    // 状态变化
    StateChanged,
    // 音频设备
    DeviceChanged,
    // 错误
    Error
};

struct Event {
    EventType type;
    uint64_t timestamp{0};
    std::string text;          // ASR 文本
    std::vector<Sample> audio; // TTS 音频
    std::string payload;       // 通用载荷（JSON 等）
};

// ========== LLM 类型 ==========
struct Message {
    std::string role;         // "system", "user", "assistant", "tool"
    std::string content;
    std::string name;         // 用于 tool 消息
};

struct ToolDef {
    std::string name;
    std::string description;
    std::string json_schema;  // JSON Schema
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

struct ToolResult {
    std::string call_id;
    std::string content;
    bool is_error{false};
};

struct LlmConfig {
    std::string model_path;
    int n_ctx{4096};
    int n_gpu_layers{0};      // 0 = CPU only
    int n_threads{4};
    float temperature{0.7f};
    int max_tokens{512};
};

// ========== ASR 类型 ==========
struct AsrConfig {
    std::string model_path;
    std::string model_type;   // "sensevoice", "paraformer", "whisper"
    int sample_rate{16000};
    bool streaming{false};
};

// ========== TTS 类型 ==========
struct TtsConfig {
    std::string model_path;
    std::string voice{"af_bella"};  // Kokoro 音色
    int sample_rate{24000};
    float speed{1.0f};
};

// ========== 搜索类型 ==========
struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
    std::string content;
    std::string published_at;
    double score{0.0};
};

struct SearchQuery {
    std::string q;
    int topk{8};
    std::string lang{"zh-CN"};
    std::optional<std::string> time_range;
    std::optional<std::string> site;
};

// ========== Memory 类型 ==========
struct MemoryItem {
    int64_t id{0};
    std::string type;         // "profile", "preference", "episodic", "semantic", "procedural"
    std::string subject;      // "user" or "agent"
    std::string content;
    double salience{1.0};
    int sensitivity{0};        // >= 1 means no cloud
    int64_t valid_from{0};
    int64_t valid_to{0};
    int64_t superseded_by{0};
    int64_t created_at{0};
    int access_count{0};
    int64_t last_access{0};
};

struct MemoryExtractResult {
    bool worth_saving{false};
    std::string type;
    std::string subject;
    std::string content;
    double confidence{0.0};
    int sensitivity{0};
    int ttl_days{365};
    std::vector<int64_t> supersedes;
};

// ========== 配置类型 ==========
struct AppConfig {
    // 音频
    int audio_device{0};
    int audio_sample_rate{48000};
    int audio_channels{1};
    int audio_buffer_ms{10};  // 回调帧长

    // VAD 参数
    float vad_threshold{0.5f};
    int vad_min_speech_ms{220};
    int vad_min_silence_ms{300};

    // 打断参数
    int barge_in_min_ms{160};
    int backchannel_max_ms{600};
    float coherence_max{0.6f};
    int fade_out_ms{40};

    // EOU 参数
    int eou_fast_ms{350};
    int eou_force_ms{900};
    int max_utterance_ms{20000};

    // 模型路径
    std::string vad_model;
    std::string asr_model;
    std::string tts_model;
    std::string llm_model;
    std::string tts_engine{"simple"};  // "simple"=系统语音(SAPI,默认)；"kokoro"=本地Kokoro模型

    // 搜索
    std::string searxng_url{"http://localhost:8080"};
    std::string tavily_key;
    std::string brave_key;

    // 记忆（M6）
    std::string memory_db_path{"memory.db"};

    // 日志
    std::string log_level{"info"};
    std::string log_file;
};

// ========== 状态机 ==========
enum class State {
    Idle,
    Listening,
    EouPending,     // 等待轮次判定
    Thinking,
    Speaking,
    Interrupting    // 瞬时状态
};

inline const char* state_to_string(State s) {
    switch (s) {
        case State::Idle: return "Idle";
        case State::Listening: return "Listening";
        case State::EouPending: return "EouPending";
        case State::Thinking: return "Thinking";
        case State::Speaking: return "Speaking";
        case State::Interrupting: return "Interrupting";
    }
    return "Unknown";
}

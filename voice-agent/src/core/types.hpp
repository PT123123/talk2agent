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
// 注意：EventType::Error 必须保持为最后一个枚举值 —— EventBus 用
// `subscriptions_[static_cast<size_t>(EventType::Error) + 1]` 定容，
// 在 Error 之后追加新事件会导致数组越界。新事件一律插在 Error 之前。
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

    // ========== R0：Conversation Runtime 事件 ==========
    // 任务生命周期（TaskManager 发射）
    TaskStarted,
    TaskProgress,
    TaskCompleted,
    TaskFailed,
    TaskCancelled,
    TaskSuperseded,
    // 工具执行（ToolWorker 发射，取代阻塞式工具调用）
    ToolStarted,
    ToolProgress,
    ToolCompleted,
    ToolFailed,
    ToolCancelled,
    // 模型生成细分（比 LlmToken 更细的时序观测点）
    ModelStarted,
    ModelFirstToken,
    // 播放
    PlaybackStarted,
    PlaybackEnded,
    // 对话语义
    UserIntent,        // 载荷为 UserSpeechIntent 名字
    TopicChanged,      // 载荷为新话题摘要
    ResponseSuperseded,// 前台回答被新轮次取代
    QuickResponse,     // 载荷为抢答/ack 文本（空 = 选择沉默）
    TimingDecision,    // 载荷为 TimingDecision 名字
    RouteDecision,     // 载荷为 ModelTier 名字
    // 错误（必须保持最后）
    Error
};

struct Event {
    EventType type;
    uint64_t timestamp{0};
    std::string text;          // ASR 文本
    std::vector<Sample> audio; // TTS 音频
    std::string payload;       // 通用载荷（JSON 等）

    // R0：跨模块关联。turn_id 关联同一轮对话，task_id 关联后台任务。
    // 缺省 0 表示"不属于任何轮次/任务"（如全局设备事件）。
    uint64_t turn_id{0};
    uint64_t task_id{0};
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
    // 打断确认期间 Agent 的音量比例（0~1）。用户开口但未达门槛时先压低，
    // 避免双方同音量互相盖住。0.35 = 压到三分之一。
    float interrupt_duck_volume{0.35f};
    // 是否启用打断门槛。false = 开口即打断（咳嗽也会掐掉回答，不推荐）。
    bool barge_in_require_threshold{true};
    // 语音输入方式："vad"=VAD 自动切分；"ptt"=按住空格/🎙 说话，松开转写。
    // 默认 ptt（手动按键）：VAD 常开麦克风意味着一直占着音频设备，
    // 且环境噪声会被当成人声；手动按键的"想说什么才按下"更可预测。
    std::string input_mode{"ptt"};

    // EOU 参数
    int eou_fast_ms{350};
    int eou_force_ms{900};
    int max_utterance_ms{20000};

    // 模型路径
    std::string vad_model;
    std::string asr_model;
    std::string tts_model;
    std::string llm_model;
    std::string tts_engine{"simple"};  // "simple"=系统语音(SAPI,默认)；"kokoro"=本地Kokoro模型；
                                       // "qwen3tts"/"chatterbox"=PyTorch 引擎（走本地 Python bridge）

    // TTS 语音参数（语速/音调/发音人）
    float tts_speed{1.0f};        // 语速倍率（0.5~2.0）
    float tts_pitch{1.0f};        // 音调倍率（保留字段，Kokoro 当前未参与生成）
    int tts_speaker_id{45};       // 发音人 ID（Kokoro 多语言 45=zf_xiaobei 中文女声）

    // §24：韵律适配器选择（"auto"=按引擎自动；"qwen3tts"/"chatterbox"=对应风格适配器）
    std::string tts_prosody_adapter{"auto"};

    // §24：PyTorch TTS 引擎的本地 Python bridge（仅 qwen3tts/chatterbox 用）
    std::string tts_bridge_endpoint{"http://127.0.0.1:8770"};
    int tts_bridge_timeout_ms{30000};       // 首次合成要加载权重，给足时间
    int tts_bridge_health_timeout_ms{1500}; // 健康探测必须快，否则卡住启动

    // ZipVoice（零样本音色克隆，纯 CPU INT8）
    // vocoder 不随模型包分发（所有音色共用），单独配。
    std::string tts_zipvoice_vocoder{"models/tts/vocos_24khz.onnx"};
    // 参考音频与其**逐字**转写。留空则用模型自带的示例参考音。
    // 二者必须严格对应，不匹配时克隆音质会明显下降。
    std::string tts_ref_audio;
    std::string tts_ref_text;
    int tts_num_steps{4};                   // 4=官方推荐；2 更快但音质降
    float tts_guidance_scale{1.5f};

    // 搜索
    std::string searxng_url{"http://localhost:8080"};
    std::string tavily_key;
    std::string brave_key;

    // ========== R7：在线强模型（OpenAI 兼容）==========
    // base_url 为空则不挂远程引擎，DEEP 档自动降级到本地 NORMAL/FAST。
    // 密钥不写入 yaml（配置文件可能进 git），只在运行时传入。
    std::string remote_base_url;
    std::string remote_model;
    int  remote_timeout_ms{30000};
    // 密钥从环境变量读，不落 yaml（配置文件可能进 git）
    std::string remote_api_key_env;
    // 远程引擎注册到哪些档位。默认只给 DEEP —— 简单问题坚决不碰网络。
    bool remote_for_deep{true};
    bool remote_for_agent{true};
    bool remote_for_search{true};

    // 记忆（M6）
    std::string memory_db_path{"memory.db"};

    // ========== Conversation Runtime 开关（会传给 Orchestrator）==========
    // 打断时把"用户实际听到的部分"记入历史并告知模型被打断。
    // 不做的话模型会以为没播的内容也说过 —— context desync。
    bool enable_interrupt_truncation{true};
    // 语义轮次判定（对标 OpenAI semantic_vad）。中文建议 low。
    bool enable_semantic_turn{true};
    // Eagerness 档位："low"(8s) / "medium"(4s) / "high"(2s)。
    // 以字符串存放，避免 core 层反向依赖 orchestrator/semantic_turn.hpp
    // （Eagerness 定义在那里）。默认 low：中文"话题在前评论在后"，
    // 决定性信息常在句末，medium 容易在句中抢话。
    std::string eagerness{"low"};
    // 对话 trace 落盘路径（JSONL）。空 = 关闭（零开销）。
    std::string trace_path;
    // 延迟指标聚合（仅内存）
    bool enable_metrics{true};

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

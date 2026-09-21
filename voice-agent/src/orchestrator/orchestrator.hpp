// src/orchestrator/orchestrator.hpp
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "core/types.hpp"
#include "core/cancel_token.hpp"
#include "core/event_bus.hpp"
#include "audio/audio_pipeline.hpp"
#include "vad/vad.hpp"
#include "asr/asr.hpp"
#include "llm/llm.hpp"
#include "tts/tts.hpp"
#include "eou_detector.hpp"
#include "smart_turn.hpp"
#include "audio_router.hpp"
#include "agent/tool_registry.hpp"
#include "agent/tools.hpp"
#include "agent/agent_loop.hpp"
#include "search/isearch.hpp"
#include "memory/memory_manager.hpp"
#include "memory/memory_command.hpp"

namespace voice_agent {

// ========== Orchestrator 事件回调 ==========
using OrchestratorCallback = std::function<void(const std::string& text)>;
// 状态机每次状态切换时回调（GUI 状态标签 / 语音轮次开始与结束判定）
using StateCallback = std::function<void(const std::string& state)>;

// ========== Orchestrator 配置 ==========
struct OrchestratorConfig {
    int eou_fast_ms = 350;
    int eou_force_ms = 900;
    int max_utterance_ms = 20000;
    int barge_in_min_ms = 160;
    int backchannel_max_ms = 600;
    int fade_out_ms = 40;
    int capture_sample_rate = 16000;  // ASR 采样率
    int playback_sample_rate = 24000;  // TTS 采样率
};

// ========== Orchestrator 主状态机 ==========
// 协调 VAD → ASR → LLM → TTS 全流程
class Orchestrator {
public:
    using Config = OrchestratorConfig;

    explicit Orchestrator(Config config = {});
    ~Orchestrator();

    // 不可复制
    Orchestrator(const Orchestrator&) = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

    // ========== 生命周期 ==========

    // 初始化所有子模块
    bool initialize(
        std::shared_ptr<AudioPipeline> audio_pipeline,
        std::shared_ptr<VAD> vad,
        std::shared_ptr<ASR> asr,
        std::shared_ptr<LLM> llm,
        std::shared_ptr<TTS> tts
    );

    // 启动状态机
    void start();

    // 停止状态机
    void stop();

    // ========== Agent 工具集成（M5）==========

    // 挂载工具注册表 + 搜索路由；挂载后 Thinking 阶段走 AgentLoop
    // （生成 → 解析工具 → 执行 → 回填 → 再生成）。
    void attach_agent(std::shared_ptr<ToolRegistry> registry,
                      std::shared_ptr<SearchRouter> search,
                      std::string system_prompt = {});

    // 是否已启用 Agent 工具循环
    bool agent_enabled() const { return agent_tools_ != nullptr; }

    // 挂载记忆管理器：自动拦截 /memory 命令、召回记忆注入 system prompt、
    // 并在每轮 Agent 完成后自动 ingest 用户事实。
    void attach_memory(std::shared_ptr<MemoryManager> memory);

    // 是否已启用记忆
    bool memory_enabled() const { return memory_ != nullptr; }

    // ========== 事件注入（供 AudioPipeline 调用）==========

    // VAD 事件
    void on_vad_speech_start(uint64_t timestamp_us);
    void on_vad_speech_end(uint64_t timestamp_us);

    // 打断检测事件
    void on_barge_in_detected();

    // ========== 回调 ==========

    // 设置文本输出回调（轮次结束时的完整回答，调用一次）
    void set_text_callback(OrchestratorCallback cb);

    // 设置流式 token 回调（生成过程中的增量片段，供实时回复面板展示）
    void set_token_callback(OrchestratorCallback cb) { token_cb_ = std::move(cb); }

    // 设置状态机状态回调（每次 set_state_ 触发）
    void set_state_callback(StateCallback cb) { state_cb_ = std::move(cb); }

    // 设置"用户说了什么"回调（ASR 转写完成的整句文本，供 GUI 回显到对话区）
    void set_user_text_callback(OrchestratorCallback cb) { user_text_cb_ = std::move(cb); }

    // 单轮阶段计时/工具事件（转发给上层 GUI 用于时间轴与“工具”区）
    void set_trace_callback(TraceFn fn) { trace_cb_ = std::move(fn); }
    void set_tool_callback(ToolReportFn fn) { tool_cb_ = std::move(fn); }

    // ========== 播报控制 ==========

    // 开关语音播报（TTS 合成/播放）。关闭时不再为流式输出合成声音，
    // 并立刻中断正在播放的声音；回答文字仍正常生成与展示。
    void set_tts_enabled(bool enabled);
    bool tts_enabled() const { return tts_enabled_.load(); }

    // ========== VAD 开关 / 手动分段 ==========

    // 开启（默认）：VAD 自动检测语音起止，SpeechEnd 后整段转写。
    // 关闭：跳过 VAD，由 begin_capture/end_capture 手动分段（ASR 直接接管）。
    void set_vad_enabled(bool enabled) { vad_enabled_.store(enabled); }
    bool vad_enabled() const { return vad_enabled_.load(); }

    // 手动开始采集（VAD 关闭时由上层在按下对讲时调用）
    void begin_capture();

    // 手动结束采集并提交转写（VAD 关闭时由上层在松开对讲时调用）
    void end_capture();

    // 获取当前状态
    State current_state() const { return state_.load(); }

    // 获取当前对话文本
    std::string current_text() const;

    // 是否正在运行
    bool is_running() const { return running_.load(); }

private:
    // ========== 状态处理 ==========
    void set_state_(State new_state);

    // 状态入口
    void enter_idle_();
    void enter_listening_();
    void enter_eou_pending_(uint64_t timestamp_us);
    void enter_thinking_(const std::string& text);
    void enter_speaking_();
    void enter_interrupting_();

    // ========== 管道操作 ==========
    void start_capture_();
    void stop_capture_();

    // 提交一段采集音频给 ASR 转写 + Agent 轮次（独立线程），
    // VAD 结束与手动 end_capture 共用。
    void submit_segment_(std::vector<int16_t> seg);

    // ========== EOU 回调 ==========
    void on_eou_(bool is_eou);

    // ========== ASR 回调 ==========
    void on_asr_result_(const ASRResult& result);

    // ========== LLM 回调 ==========
    void on_llm_token_(const LLMResponse& chunk);
    void on_llm_complete_();

    // ========== TTS 回调 ==========
    void on_tts_chunk_(const int16_t* audio, size_t frames, bool is_last);

    // ========== 成员 ==========
    Config config_;
    std::atomic<bool> running_{false};
    std::atomic<State> state_{State::Idle};
    std::string accumulated_text_;

    // 语音段采集：SpeechStart→SpeechEnd 期间累积 16kHz PCM，段末交给 ASR 整段转写。
    std::atomic<bool> capturing_{false};
    std::vector<int16_t> speech_buffer_;
    std::atomic<bool> turn_busy_{false};  // 语音轮次处理中（防重叠线程）

    // 子模块
    std::shared_ptr<AudioPipeline> audio_pipeline_;
    std::shared_ptr<VAD> vad_;
    std::shared_ptr<ASR> asr_;
    std::shared_ptr<LLM> llm_;
    std::shared_ptr<TTS> tts_;

    // 内部模块
    EOUDetector eou_detector_;
    SmartTurn smart_turn_;
    AudioRouter audio_router_;

    // 取消令牌
    CancelToken::Ptr session_token_;
    mutable std::mutex text_mutex_;

    // 回调
    OrchestratorCallback text_callback_;    // 完整回答（每轮一次）
    StateCallback state_cb_;
    OrchestratorCallback user_text_cb_;
    OrchestratorCallback token_cb_;         // 流式增量片段（可空）
    TraceFn trace_cb_;
    ToolReportFn tool_cb_;

    // 播报开关
    std::atomic<bool> tts_enabled_{true};
    uint64_t user_speech_start_us_{0};   // 最近一次语音段开始时刻（微秒）
    std::atomic<double> tts_accum_ms_{0.0}; // 流式 TTS 累计耗时
    bool tts_accum_active_{false};       // 是否正在累计流式 TTS 耗时

    // VAD 开关（默认开启）。关闭后跳过 vad_->process，改用手动分段。
    std::atomic<bool> vad_enabled_{true};
    uint64_t capture_start_us_{0};       // 手动采集开始时刻（微秒）

    // Agent 工具（M5，可选）
    std::shared_ptr<ToolRegistry> agent_tools_;
    std::shared_ptr<SearchRouter> search_router_;
    std::string agent_system_prompt_;

    // 记忆（M6，可选）
    std::shared_ptr<MemoryManager> memory_;
};

}  // namespace voice_agent

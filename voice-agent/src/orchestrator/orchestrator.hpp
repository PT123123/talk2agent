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

    // 设置文本输出回调
    void set_text_callback(OrchestratorCallback cb);

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
    OrchestratorCallback text_callback_;

    // Agent 工具（M5，可选）
    std::shared_ptr<ToolRegistry> agent_tools_;
    std::shared_ptr<SearchRouter> search_router_;
    std::string agent_system_prompt_;

    // 记忆（M6，可选）
    std::shared_ptr<MemoryManager> memory_;
};

}  // namespace voice_agent

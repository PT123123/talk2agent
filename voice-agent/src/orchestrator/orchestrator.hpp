// src/orchestrator/orchestrator.hpp
#pragma once
#include <atomic>
#include <functional>
#include <map>
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
#include "core/task.hpp"
#include "core/working_context.hpp"
#include "orchestrator/user_intent.hpp"
#include "orchestrator/response_policy.hpp"
#include "orchestrator/turn_admission.hpp"
#include "orchestrator/fast_response.hpp"
#include "orchestrator/background_cache.hpp"
#include "orchestrator/model_router.hpp"
#include "orchestrator/remote_llm.hpp"
#include "tts/prosody.hpp"
#include "tts/response_plan.hpp"
#include "tts/tts_adapter.hpp"
#include "orchestrator/interruption_truncation.hpp"
#include "orchestrator/semantic_turn.hpp"
#include "util/voice_trace.hpp"
#include "util/latency_metrics.hpp"

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

    // ========== R1：Conversation Runtime ==========
    // 后台/投机任务用的 worker 数。前台轮次默认仍在本线程同步跑（保序），
    // 这些 worker 服务搜索/记忆/预取。
    int runtime_workers = 3;
    // 打断后仍继续跑完（结果入 cache 不播报）的任务类型开关。
    bool background_continue_on_interrupt = true;
    // 启用 ResponsePolicy：把"要不要说"从 LLM 里剥出来。
    bool enable_response_policy = true;
    // 换话题时是否自动 supersede 同 topic 的后台任务。
    bool enable_topic_supersede = true;
    // 慢任务时是否先说一句短回应（Fast Response Layer）。
    bool enable_fast_response = true;
    // 抢答阈值：预计耗时超过它才先说一句。0 = 用 FastResponseLayer 默认值。
    int ack_threshold_ms = 700;

    // ========== R9a：打断上下文同步 ==========
    // 打断时把"用户实际听到的部分"记入对话历史，并告知模型它被打断了。
    // 不做的话模型会以为没播的内容也说过 —— context desync。
    bool enable_interrupt_truncation = true;

    // ========== R9b：语义轮次判定 ==========
    // 对标 OpenAI semantic_vad。中文建议 low（中文"话题在前评论在后"，
    // 决定性信息常在句末）。
    bool enable_semantic_turn = true;
    Eagerness eagerness{Eagerness::Low};

    // ========== R8：可观测 ==========
    // 对话 trace 落盘路径（JSONL）。空 = 关闭（零开销）。
    std::string trace_path;
    // 是否采集延迟指标（内存聚合，不落盘）
    bool enable_metrics = true;
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

    // ========== R6：韵律 / 响应修订 ==========

    // 设置本轮情绪。影响 ProsodyPlanner 生成的韵律偏移。
    // 传空字符串或 intensity<=0 表示中性（推荐：宁中性勿半吊子）。
    void set_emotion(std::string emotion, float intensity = 0.6f);

    // 放弃本轮尚未播出的内容（Response Revision）。
    // 典型场景：模型改主意、或用户抢话后需要立即让出话轮。
    size_t discard_unplayed();

    // 本轮回答是否已全部播完
    bool response_finished() const { return response_plan_.finished(); }

    // 响应修订统计（appended / played / discarded / revised_rounds）
    ResponsePlan::Stats response_stats() const { return response_plan_.stats(); }

    // 韵律规划器（调参/测试用）
    ProsodyPlanner& prosody() { return prosody_planner_; }

    // ========== R8：可观测接口 ==========

    // 延迟指标（聚合）
    LatencyMetrics& metrics() { return metrics_; }
    const LatencyMetrics& metrics() const { return metrics_; }

    // Trace 写盘器（未启用时为 nullptr）
    VoiceTraceWriter* trace() { return trace_writer_.get(); }

    // 人类可读的延迟摘要（一行）
    std::string latency_summary() const { return metrics_.summary_line(); }

    // 是否已开启 trace
    bool trace_enabled() const { return trace_writer_ != nullptr && trace_writer_->ok(); }

    // ========== R1：Conversation Runtime 对外接口 ==========

    // 任务管理器（后台搜索/记忆/预取都挂在上面）
    TaskManager& tasks() { return tasks_; }

    // 上下文管理器（WorkingContext + 分层拼装）
    ContextManager& context() { return context_; }

    ResponsePolicy& policy() { return admission_.policy(); }

    // 轮次准入决策器（调试/测试用）
    TurnAdmission& admission() { return admission_; }

    // 后台结果缓存（调试/长跑观测用）
    BackgroundCache& background_results() { return bg_results_; }

    // Fast Response 层（调试/调参用）
    FastResponseLayer& fast_response() { return fast_response_; }

    // 模型路由器（换档/统计/降级观测）
    ModelRouter& model_router() { return model_router_; }

    // 最近一次路由结果（GUI/trace 用）
    RouteResult last_route() const {
        std::lock_guard<std::mutex> lock(policy_mutex_);
        return last_route_;
    }

    // R4：注册一个模型引擎到指定档位。在 initialize() 之后调用。
    void register_model_engine(ModelTier tier, std::shared_ptr<IModelEngine> engine);

    // R7：挂载在线强模型（OpenAI 兼容端点）。挂上后 DEEP/AGENT/SEARCH
    // 档会走远程；未挂或远程不可用时 ModelRouter 自动降级到本地。
    void attach_remote_llm(const RemoteLLMConfig& cfg,
                           const std::vector<ModelTier>& tiers);

    // 远程引擎（未挂载则为 nullptr）
    std::shared_ptr<RemoteLLM> remote_llm() const { return remote_; }

    // 最近一次用户话语意图（GUI/trace 用）
    UserSpeechIntent last_user_intent() const {
        return admission_.last_intent();
    }

    // 最近一次 ResponsePolicy 判定（GUI/trace 用）
    std::string last_policy_reason() const;

    // 提交一段用户文本走完整轮次（GUI 文本输入 / 测试用）。
    // 与语音路径共用同一套意图分类 + Policy 决策。
    void submit_user_text(std::string text);

    // 提交一个后台任务（搜索/记忆/预取）。topic_key 用于换话题时 supersede。
    TaskId submit_background_task(std::string name, std::string topic_key,
                                  std::function<TaskOutcome(TaskContext&)> work);

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

    // 真正执行打断：取消当前 LLM、中断 TTS 合成与播放、清空待播音频，
    // 并切回 Listening。供 VAD 检测打断、手动对讲(begin_capture)与淡出回调共用。
    // R1：不再一刀切 —— 前台回答按 policy 取消，后台任务按各自 policy
    // 分派（cancel / pause / continue / supersede）。
    void interrupt_agent_();

    // R1：把「用户说了什么」交给 admission_ 决策，再执行其结论。
    // 返回 true 表示应当继续走 LLM 轮次。
    bool admit_user_turn_(const std::string& text);

    // R1：把召回记忆 / WorkingContext 拼进 system prompt。
    // 非 const：会把召回结果写进 ContextManager 的 P5 层。
    std::string build_system_prompt_with_context_(const std::string& user_text);

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

    // 流式播放状态：Thinking 期间首段音频就已开始播放，
    // 无需等 LLM 全部生成完（避免 TTS 憋满整段才开口）。
    std::atomic<bool> speech_started_{false};

    // R6：韵律规划 + 分段生命周期。
    // 替换原先的 stream_sentence_ 裸缓冲 —— 现在文本先进 ResponsePlan，
    // 由 ProsodyPlanner 切句并加韵律，再经 ITtsAdapter 落到具体引擎。
    ProsodyPlanner prosody_planner_;
    ResponsePlan response_plan_;
    std::shared_ptr<ITtsAdapter> tts_adapter_;
    // 已送 TTS 的段数（ResponsePlan 的段下标），用于状态回调
    std::atomic<size_t> synth_cursor_{0};
    // 还没到句子边界的半句缓冲
    std::string speech_buffer_pending_;
    // 本轮情绪（由 Policy/GUI 设置；为空 = 中性）
    std::mutex emotion_mutex_;
    std::string current_emotion_;
    float current_emotion_intensity_{0.0f};

    // R8：可观测。metrics_ 无条件构造（未启用时不 record，内存开销可忽略）。
    LatencyMetrics metrics_;
    // trace 需在 initialize() 里按 config_.trace_path 打开，故用 unique_ptr 延迟构造
    std::unique_ptr<VoiceTraceWriter> trace_writer_;

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
    // 本轮用户输入（on_llm_complete_ 时与助手回答一起进 ContextManager）
    std::string last_user_text_;

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

    // ========== R1：Conversation Runtime ==========
    // 后台任务执行池。前台轮次仍在本线程同步跑（保序 + 首响应最快），
    // 搜索/记忆/预取挂在 tasks_ 上互不阻塞。
    TaskManager tasks_;

    // WorkingContext + P0~P7 分层上下文。
    // 成员顺序有意义：admission_ 的构造要取 &context_.working()，
    // 所以 context_ 必须先于 admission_ 声明。
    ContextManager context_;

    // 「要不要起这一轮」的唯一决策源（意图分类 + ResponsePolicy + 换话题）。
    // Orchestrator 只负责执行它的结论，不重复实现规则 —— 两处规则一定会漂移。
    TurnAdmission admission_;

    // 意图分类器：把"嗯/对/好"从正常发言里区分出来（由 admission_ 持有）

    // 最近一次 Policy 理由（供 GUI 与 trace 观测）
    mutable std::mutex policy_mutex_;
    std::string last_policy_reason_;
    std::string last_policy_action_;

    // 后台任务结果的落地处：入 cache 而不是直接播报。
    void absorb_background_result_(TaskId id, const TaskOutcome& outcome);

    // 当前轮次 id（写进事件，串联 trace）
    std::atomic<uint64_t> current_turn_id_{0};

    // 后台结果缓存：supersede 掉的任务结果落地处。可被后续轮次引用，
    // 但不会主动播报抢话。
    BackgroundCache bg_results_;

    // 慢任务先说一句（"我看一下。"）+ 自适应延迟估计
    FastResponseLayer fast_response_;

    // R4：Model Tier 路由。决定这一轮用哪一档（FAST/NORMAL/DEEP/...）
    // 与对应的采样参数。
    ModelRouter model_router_;

    // 最近一次路由结果（供 GUI 与 trace）
    mutable RouteResult last_route_;

    // R7：在线强模型。未挂载 = 纯本地模式，DEEP 档会走采样参数区分。
    std::shared_ptr<RemoteLLM> remote_;
    // 本轮路由结果是否落在"远程引擎已注册"的档位上。
    // 由 enter_thinking_ 的路由段设置，决定走远程还是本地。
    bool route_tier_uses_remote_{false};

    // R3：按 Policy 结论派生后台任务。需要 search/agent 时把耗时工作
    // 挪到后台，前台不等。返回创建的任务 id（kInvalidTaskId = 未派生）。
    TaskId spawn_background_for_decision_(const ResponseDecision& decision,
                                          const std::string& user_text);

    // R3：抢答。把一句短回应直接推给 TTS（不进 LLM、不进对话历史）。
    void speak_ack_(const std::string& text);

    // R3：把可用的后台结果注入 ContextManager 的 P6 层（供本轮引用）。
    void inject_cached_results_(const std::string& user_text);

    // R9a：打断时同步上下文 —— 记录用户实际听到的部分 + 告知模型被打断。
    void sync_interrupted_context_();

    // R6：把 LLM 增量文本切成段、经韵律规划后送 TTS。
    // 替换原先的 stream_sentence_ 裸缓冲。
    void feed_speech_text_(const std::string& token);

    // R6：取出下一个待合成段并交给 adapter。返回是否成功送入。
    bool pump_next_segment_();

    // R6：把残留的未到标点文本作为尾句补进来（LLM 结束时调用）。
    void flush_speech_tail_();

    // R8：记录一条时序事件到本轮 trace
    void trace_event_(const std::string& type, const std::string& detail = {});

    // R8：收尾本轮 trace（LLM 结束时调用）。把决策 + 延迟指标写盘。
    void finish_turn_trace_();

    // R8：本轮起点的时戳（用于事件相对时间）
    int64_t turn_start_ns_{0};
    std::vector<TraceEvent> turn_events_;
    // 本轮关键时点
    int64_t turn_asr_done_ns_{0};
    int64_t turn_llm_start_ns_{0};
    int64_t turn_first_token_ns_{0};
    int64_t turn_first_audio_ns_{0};
    std::atomic<bool> turn_interrupted_{false};
    double barge_in_start_ms_{0.0};   // R8：用户开口时刻（打断延迟基准）
    // R9a：打断截断器
    InterruptionTruncation truncator_;
    // R9b：语义轮次判定
    SemanticTurnDetector semantic_turn_;
    // 上一轮被中断的说明（写进下一轮的 P2 任务状态层）
    std::string pending_interrupt_note_;
    std::string turn_user_text_;
    std::string turn_ack_text_;
};

}  // namespace voice_agent

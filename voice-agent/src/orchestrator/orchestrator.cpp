// src/orchestrator/orchestrator.cpp
#include "orchestrator.hpp"
#include "util/log.hpp"
#include "orchestrator/model_engines.hpp"
#include "tts/qwen3_tts_adapter.hpp"
#include "tts/chatterbox_adapter.hpp"
#include <cctype>
#include <cstring>
#include <chrono>

namespace voice_agent {

namespace {
inline double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 句子内是否含非空白/标点的实际内容（排除纯符号/空白片段）
bool sentence_has_content(const std::string& s) {
    for (unsigned char c : s) {
        if (c > 0x7F) return true;              // 含任意非 ASCII（中文等）
        if (!std::isspace(c)) {
            if (strchr("，。！？；：、,.!?;:()[]{}《》“”‘’\"'-", c) == nullptr)
                return true;
        }
    }
    return false;
}
// 注：原先的 find_sentence_boundary() 与 utf8_hold_back() 已移除 ——
// 句级切分统一由 ProsodyPlanner::split_sentences 负责（它还处理
// 连续标点合并、纯标点段丢弃、超长文本二次切分），两处实现并存必然漂移。
}  // namespace

// ========== 构造函数 / 析构函数 ==========

Orchestrator::Orchestrator(Config config)
    : config_(config)
    , eou_detector_(EOUDetector::Config{
          config.eou_fast_ms,
          config.eou_force_ms,
          config.max_utterance_ms})
    , smart_turn_(SmartTurn::Config{
          config.barge_in_min_ms,
          config.backchannel_max_ms,
          0.6f})
    , tasks_(std::max(1, config.runtime_workers))
    , admission_(TurnAdmissionConfig{
          /*enable_response_policy*/config.enable_response_policy,
          /*enable_topic_supersede*/config.enable_topic_supersede,
          /*agent_speaking*/false},
          &context_.working())   // 与 ContextManager 共享同一份 WorkingContext
    , fast_response_(FastResponseLayer::Config{
          /*enabled*/config.enable_fast_response,
          /*ack_threshold_ms*/config.ack_threshold_ms > 0
                                ? config.ack_threshold_ms : 700,
          /*default_estimate_ms*/600,
          /*max_acks_per_task*/1,
          /*ewma_alpha*/0.3})
    , truncator_(InterruptionTruncation::Config{
          /*min_played_ratio*/0.02,
          /*interrupt_marker*/"（被用户打断）",
          /*append_marker*/true})
    , semantic_turn_(SemanticTurnDetector::Config{
          /*eagerness*/config.eagerness,
          /*short_utterance_chars*/6})
{
    // 打断门槛的两个标量在构造函数体里赋值：它们声明在 vad_enabled_ 附近，
    // 早于 truncator_/semantic_turn_，放进初始化列表会触发 /W3 的重排警告。
    interrupt_duck_volume = config.interrupt_duck_volume;
    barge_in_require_threshold = config.barge_in_require_threshold;

    // 后台任务结果一律入cache / WorkingContext，绝不主动抢话播报。
    // 播报与否由上层按 turn 上下文决定 —— 这里只负责"结果有地方放"。
    tasks_.set_result_sink([this](const TaskId& id, const TaskOutcome& outcome) {
        absorb_background_result_(id, outcome);
    });

    // R6：TTS 适配器。SAPI 与模型引擎的参数能力不同，映射方式也不同。
    // 引擎在 initialize() 时才知道，故先建一个默认的（模型引擎），
    // initialize() 会按实际引擎重建。
    tts_adapter_ = std::make_shared<ModelTtsAdapter>(nullptr);

    // R8：trace 写盘器。路径为空则关闭（零开销）。
    if (!config_.trace_path.empty()) {
        trace_writer_ = std::make_unique<VoiceTraceWriter>(config_.trace_path, true);
    }
}

Orchestrator::~Orchestrator() {
    stop();
}

// ========== 初始化 ==========

bool Orchestrator::initialize(
    std::shared_ptr<AudioPipeline> audio_pipeline,
    std::shared_ptr<VAD> vad,
    std::shared_ptr<ASR> asr,
    std::shared_ptr<LLM> llm,
    std::shared_ptr<TTS> tts
) {
    audio_pipeline_ = std::move(audio_pipeline);
    vad_ = std::move(vad);
    asr_ = std::move(asr);
    llm_ = std::move(llm);
    tts_ = std::move(tts);

    // EOU 回调
    eou_detector_.set_callback([this](bool is_eou) {
        on_eou_(is_eou);
    });

    // ASR 回调
    if (asr_) {
        asr_->set_callback([this](const ASRResult& result) {
            on_asr_result_(result);
        });
    }

    // AudioRouter 打断淡出：fade_out_ms 内把缓冲尾巴淡到 0，避免硬切爆音。
    // （原先这里挂的是 fade_out_callback → on_barge_in_detected，但 AudioRouter
    //  从不触发淡出，那个回调是死代码；打断实际由 on_vad_speech_start 直接调用。）
    audio_router_.set_fade_out_ms(config_.fade_out_ms);
    audio_router_.set_output_rate(
        tts_ ? tts_->config().sample_rate : config_.playback_sample_rate);

    // R4：把现有 LLM 包装成引擎注册到各档位。
    // 同一个本地模型可以服务多档 —— 靠采样参数区分（详见 ModelRouter）。
    // 这样"不把所有问题路由到最大模型"就能落地：简单问题用低温度+短输出，
    // 复杂推理才放开长度。
    if (llm_) {
        auto engine = std::make_shared<LocalLlamaEngine>(llm_, /*supports_tools=*/true);
        // 用数组而非花括号列表：MSVC 对 range-for 的花括号列表
        // 在某些包含级别下会报 "is not a class or namespace name"。
        const ModelTier tiers[] = {
            ModelTier::Fast, ModelTier::Normal, ModelTier::Deep,
            ModelTier::Agent, ModelTier::Search, ModelTier::Background
        };
        for (ModelTier t : tiers) {
            model_router_.register_engine(t, engine);
        }
    }

    // R6：按实际引擎建 TTS 适配器 —— SAPI 的参数能力与 Kokoro 不同。
    // §24：prosody_adapter 显式指定风格适配器（qwen3tts / chatterbox）时优先采用，
    // 否则按引擎类型自动选（SAPI→SapiTtsAdapter，模型引擎→ModelTtsAdapter）。
    if (tts_) {
        // 注意读的是 **requested** 引擎而不是 config().engine：
        // bridge 探测失败时 TTS::initialize 会把 engine 就地降级成 "simple"，
        // 但用户配的仍是 qwen3tts，韵律语义应当按 qwen3tts 理解
        // （instruction 照算，只是最终由 SAPI 发声）。
        const std::string requested = tts_->requested_engine();
        const std::string pa = tts_->config().prosody_adapter;
        if (pa == "qwen3tts" || requested == "qwen3tts") {
            tts_adapter_ = std::make_shared<Qwen3TtsAdapter>(tts_.get());
        } else if (pa == "chatterbox" || requested == "chatterbox") {
            tts_adapter_ = std::make_shared<ChatterboxAdapter>(tts_.get());
        } else {
            tts_adapter_ = tts_->simple_engine()
                               ? std::static_pointer_cast<ITtsAdapter>(
                                     std::make_shared<SapiTtsAdapter>(tts_.get()))
                               : std::static_pointer_cast<ITtsAdapter>(
                                     std::make_shared<ModelTtsAdapter>(tts_.get()));
        }
        LOG_INFO("TTS adapter: {} (full prosody={}, engine={}, voice={})",
                 tts_adapter_->name(),
                 tts_adapter_->supports_full_prosody() ? "yes" : "no",
                 tts_->engine_name(), tts_->provider_label());
    }

    LOG_INFO("Orchestrator initialized");
    return true;
}

void Orchestrator::register_model_engine(ModelTier tier,
                                         std::shared_ptr<IModelEngine> engine) {
    model_router_.register_engine(tier, std::move(engine));
}

void Orchestrator::attach_remote_llm(const RemoteLLMConfig& cfg,
                                     const std::vector<ModelTier>& tiers) {
    if (cfg.base_url.empty() || cfg.model.empty()) {
        LOG_INFO("Remote LLM not attached (empty base_url or model) - "
                 "DEEP tier stays local");
        return;
    }
    remote_ = std::make_shared<RemoteLLM>(cfg);
    if (!remote_->available()) {
        LOG_WARN("Remote LLM not available after construction");
        remote_.reset();
        return;
    }
    for (ModelTier t : tiers) {
        model_router_.register_engine(t, remote_);
    }
    LOG_INFO("Remote LLM attached: {} model={} tiers=[{}]",
             remote_->name(), cfg.model,
             [&] {
                 std::string s;
                 for (auto t : tiers) {
                     if (!s.empty()) s += ",";
                     s += model_tier_to_string(t);
                 }
                 return s;
             }());
}

// ========== 生命周期 ==========

void Orchestrator::start() {
    if (running_.exchange(true)) return;

    session_token_ = std::make_shared<CancelToken>();
    tasks_.start();   // R1：后台任务池（可重复 start，幂等）

    // 注册 VAD 回调
    if (vad_) {
        vad_->set_callback([this](VADEvent event, const int16_t* data, size_t frames) {
            (void)data; (void)frames;
            auto now = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count() / 1000);
            switch (event) {
                case VADEvent::SpeechStart:
                    on_vad_speech_start(now);
                    break;
                case VADEvent::SpeechEnd:
                    on_vad_speech_end(now);
                    break;
                default:
                    break;
            }
        });
    }

    // 注册 AudioPipeline 输入回调（采集数据流向 VAD，同时缓冲语音段）
    if (audio_pipeline_) {
        audio_pipeline_->set_input_callback([this](const int16_t* data, size_t frames) {
            // 下采样 48kHz → 16kHz
            std::vector<int16_t> pcm_16k(frames / 3);
            for (size_t i = 0; i < pcm_16k.size(); ++i) {
                int32_t sum = 0;
                for (int j = 0; j < 3; ++j) sum += data[i * 3 + j];
                pcm_16k[i] = static_cast<int16_t>(sum / 3);
            }

            // 采集进行中：累积当前语音段（SpeechStart → SpeechEnd）
            if (capturing_.load()) {
                speech_buffer_.insert(speech_buffer_.end(),
                                      pcm_16k.begin(), pcm_16k.end());
                if (speech_buffer_.size() > 16000u * 30u) {  // 30s 上限
                    speech_buffer_.erase(speech_buffer_.begin(),
                                         speech_buffer_.end() - 16000u * 30u);
                }
            }

            if (vad_enabled_.load() && vad_) {
                vad_->process(pcm_16k.data(), pcm_16k.size());
            }

            // 打断门槛轮询：音频回调是唯一稳定的周期性入口（每 10ms 一次），
            // 放这里才能在 duck 期间判断"用户是否已经说了够久"。
            if (barge_in_pending_.load()) poll_barge_in_threshold_();
        });

        // 注册 AudioPipeline 播放回调（从 AudioRouter 拉取 TTS 音频）
        audio_pipeline_->set_playback_callback([this](int16_t* data, size_t frames) {
            if (audio_router_.is_playing()) {
                if (!audio_router_.get_playback_frames(data, frames)) {
                    // 缓冲放完：可能是自然播完，也可能是打断淡出走完。
                    // 只有还在 Speaking 才是"自然播完"—— 打断路径已经把状态
                    // 推到 Interrupting → Listening，此时再 enter_idle_()
                    // 会把用户正在说话的轮次直接踢回 Idle（表现为mic 停了）。
                    const bool natural_end = (state_.load() == State::Speaking);
                    // simple(SAPI) 引擎的朗读不经过 AudioRouter（合成回调
                    // 只有 is_last、0 帧），缓冲天然是空的。它还在出声时
                    // 绝不能收尾：状态机先于语音结束的话，token 喂入门控
                    //（Thinking|Speaking）会把后续句子全部挡掉 —— 表现为
                    // 只读第一句。这里保持播放模式、输出静音等它读完。
                    if (natural_end && tts_ && tts_->is_speaking()) {
                        std::memset(data, 0, frames * sizeof(int16_t));
                        return;
                    }
                    audio_router_.stop_playback();
                    std::memset(data, 0, frames * sizeof(int16_t));
                    if (natural_end) enter_idle_();
                }
            } else {
                std::memset(data, 0, frames * sizeof(int16_t));
            }
        });

        audio_pipeline_->start();
    }

    enter_idle_();
    LOG_INFO("Orchestrator started");
    LOG_INFO("{}", metrics_.summary_line());

    // R8：写会话头，便于离线回放时知道这是哪套配置
    if (trace_writer_ && trace_writer_->ok()) {
        nlohmann::json info;
        info["tts_engine"] = tts_ ? tts_->engine_name() : "none";
        info["llm_provider"] = llm_ ? llm_->provider_label() : "none";
        info["remote_attached"] = remote_ != nullptr;
        info["runtime_workers"] = config_.runtime_workers;
        info["barge_in_budget_ms"] = 250;
        trace_writer_->write_session_header(info);
    }
}

void Orchestrator::stop() {
    if (!running_.exchange(false)) return;

    if (session_token_) {
        session_token_->cancel();
        session_token_.reset();
    }

    // R1：先停后台任务池，再停音频 —— 反序会让 worker 在 audio_pipeline
    // 已停后仍尝试写 TTS/EventBus。
    tasks_.stop();

    if (audio_pipeline_) {
        audio_pipeline_->stop();
    }

    if (llm_) {
        llm_->stop();
    }

    if (tts_) {
        tts_->stop();
    }

    capturing_ = false;
    speech_buffer_.clear();
    eou_detector_.reset();
    smart_turn_.reset();
    // 会话停了也要撤掉 duck，否则下次 start() 接手的是被压低的音量
    barge_in_pending_.store(false);
    barge_in_confirmed_.store(false);
    audio_router_.stop_playback();

    LOG_INFO("Orchestrator stopped");
}

// ========== 事件注入 ==========

void Orchestrator::on_vad_speech_start(uint64_t timestamp_us) {
    if (!running_.load()) return;

    user_speech_start_us_ = timestamp_us;

    State s = state_.load();
    LOG_DEBUG("VAD speech start in state {}", state_to_string(s));

    // 开始累积语音段
    capturing_ = true;
    speech_buffer_.clear();

    if (s == State::Idle || s == State::Listening) {
        enter_listening_();
    }

    eou_detector_.on_speech_start(timestamp_us);
    smart_turn_.user_speech_start(timestamp_us);

    // Agent 正在播报时用户开口 —— 这是"用户在尝试插话"的信号。
    //
    // 旧实现是开口即 interrupt_agent_()，结果咳嗽/咂嘴/"嗯"这类短促音
    // 也会把回答掐掉，而 barge_in_min_ms 这个配置对打断路径完全没有作用
    //（SmartTurn::evaluate_barge_in 全仓库零调用点）。
    //
    // 现在改为：先 duck 音量 + 计时，持续到 barge_in_min_ms 才真打断。
    // 门槛未到就结束语音（见 on_vad_speech_end）则放弃，音量复原。
    if (s == State::Speaking && smart_turn_.is_agent_speaking()) {
        barge_in_start_ms_ = now_ms();   // R8：打断延迟计时起点
        if (barge_in_require_threshold && config_.barge_in_min_ms > 0) {
            barge_in_pending_.store(true);
            barge_in_confirmed_.store(false);
            barge_in_pending_start_ms_ = now_ms();
            audio_router_.set_output_gain(interrupt_duck_volume);
            LOG_DEBUG("Barge-in pending: duck to {:.2f}, wait {} ms",
                      static_cast<double>(interrupt_duck_volume),
                      config_.barge_in_min_ms);
        } else {
            interrupt_agent_();
        }
    }
}

bool Orchestrator::poll_barge_in_threshold_() {
    if (!barge_in_pending_.load()) return false;
    if (state_.load() != State::Speaking) {
        // 状态已经被别的事件推走（回答自然播完/ 已被取消），门槛作废
        cancel_barge_in_pending_();
        return false;
    }
    if (now_ms() - barge_in_pending_start_ms_ < config_.barge_in_min_ms) {
        return false;   // 还没到门槛，继续 duck
    }
    barge_in_confirmed_.store(true);
    barge_in_pending_.store(false);
    audio_router_.set_output_gain(1.0f);   // 确认要停了，音量不必再压
    interrupt_agent_();
    return true;
}

void Orchestrator::cancel_barge_in_pending_() {
    if (!barge_in_pending_.exchange(false)) return;
    audio_router_.set_output_gain(1.0f);
    LOG_DEBUG("Barge-in cancelled: speech too short (<{} ms)",
              config_.barge_in_min_ms);
}

void Orchestrator::set_barge_in_threshold(bool enabled, int min_ms,
                                          float duck_volume) {
    // min_ms <= 0 视作"不要门槛"，避免 GUI 传 0 后卡在永远打不断的状态
    barge_in_require_threshold = enabled && min_ms > 0;
    config_.barge_in_min_ms = std::max(0, min_ms);
    interrupt_duck_volume = std::clamp(duck_volume, 0.0f, 1.0f);
    // 门槛被关小/ 关掉时，正在等待的打断立即按新规则裁决
    if (!barge_in_require_threshold) {
        cancel_barge_in_pending_();
    } else if (barge_in_pending_.load()) {
        poll_barge_in_threshold_();
    }
    // 注意：这里不能直接把 const char* 传给 LOG_*，fmt 会把它当成格式串
    // 再解析一遍（"on"/"off" 不是合法格式串 → C3615）。包一层 std::string。
    LOG_INFO("Barge-in threshold: {} (min {} ms, duck {:.2f})",
             std::string(barge_in_require_threshold ? "on" : "off"),
             config_.barge_in_min_ms, static_cast<double>(interrupt_duck_volume));
}

void Orchestrator::on_vad_speech_end(uint64_t timestamp_us) {
    if (!running_.load()) return;

    // 语音段结束：停止累积，把整段音频交给 ASR（Whisper 整段转写）
    capturing_ = false;
    if (user_speech_start_us_ != 0) {
        const double speech_ms = (timestamp_us - user_speech_start_us_) / 1000.0;
        if (trace_cb_) trace_cb_("speech", speech_ms);
        user_speech_start_us_ = 0;
    }

    eou_detector_.on_vad_end(timestamp_us);
    smart_turn_.user_speech_end(timestamp_us);

    // 语音段结束但门槛还没到 → 这是咳嗽/咂嘴/"嗯"，不是插话。
    // 放弃打断并把音量复原，让 Agent 把话说完。
    if (barge_in_pending_.load() && !barge_in_confirmed_.load()) {
        cancel_barge_in_pending_();
    }

    std::vector<int16_t> seg;
    seg.swap(speech_buffer_);
    submit_segment_(std::move(seg));
}

// 手动采集开始（VAD 关闭 / ASR 接管模式）：对讲按下时调用
void Orchestrator::begin_capture() {
    if (!running_.load()) return;

    capturing_ = true;
    speech_buffer_.clear();
    capture_start_us_ = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() / 1000);

    State s = state_.load();
    if (s == State::Idle || s == State::Listening) enter_listening_();

    // 按键是明确意图，不需要等门槛。若此前 VAD 侧正在 duck 等待，先撤掉。
    cancel_barge_in_pending_();

    // 手动对讲（VAD 关闭）：Agent 正在播报时按下对讲即打断，
    // 立即停止当前 LLM 与语音播报，避免"界面已打断但声音仍在响"。
    if (s == State::Speaking && smart_turn_.is_agent_speaking()) {
        barge_in_start_ms_ = now_ms();   // R8：打断延迟计时起点
        interrupt_agent_();
    }
}

// 手动采集结束（VAD 关闭 / ASR 接管模式）：对讲松开时调用，整段提交转写
void Orchestrator::end_capture() {
    if (!running_.load()) return;

    capturing_ = false;
    if (capture_start_us_ != 0) {
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() / 1000);
        const double speech_ms = (now - capture_start_us_) / 1000.0;
        if (trace_cb_) trace_cb_("speech", speech_ms);
        capture_start_us_ = 0;
    }

    std::vector<int16_t> seg;
    seg.swap(speech_buffer_);
    submit_segment_(std::move(seg));
}

void Orchestrator::submit_segment_(std::vector<int16_t> seg) {
    if (turn_busy_.exchange(true)) {
        // 上一轮仍在处理（LLM 生成中），丢弃本次语音段，避免线程堆积
        speech_buffer_.clear();
        LOG_WARN("Turn processing busy - dropping utterance");
        return;
    }

    // 转写 + Agent 轮次放到独立线程，避免阻塞音频回调线程
    // （ASR 回调会把转写文本追加进 accumulated_text_，此处随后统一取出）
    std::thread([this, seg = std::move(seg)]() {
        std::string text;
        if (asr_ && !seg.empty()) {
            const double t0 = now_ms();
            text = asr_->transcribe_segment(seg.data(), seg.size());
            if (trace_cb_) trace_cb_("asr", now_ms() - t0);
            // R8：转写耗时（语音结束 → 文本）
            metrics_.record(MetricKind::SpeechToAsr, now_ms() - t0);
            LOG_INFO("speech_end: ASR transcribed {} chars", text.size());
        }
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            if (text.empty()) text = accumulated_text_;
            accumulated_text_.clear();   // 用户文本就此提交，后续由 LLM 流式回填
        }
        if (!running_.load()) {
            turn_busy_ = false;
            return;
        }
        LOG_INFO("[Orch] 语音轮提交用户文本 len={} turn={}（先回显再进入 LLM）",
                 text.size(), current_turn_id_.load());
        if (user_text_cb_ && !text.empty()) user_text_cb_(text);
        if (text.empty()) {
            enter_idle_();
        } else if (!admit_user_turn_(text)) {
            // Policy / 意图判定为不需要起新轮次（附和、静默窗口等）
            turn_busy_ = false;
            enter_listening_();
        } else {
            enter_thinking_(text);
        }
        turn_busy_ = false;
    }).detach();
}

void Orchestrator::on_barge_in_detected() {
    interrupt_agent_();
}

void Orchestrator::interrupt_agent_() {
    if (!running_.load()) return;

    LOG_WARN("Barge-in detected, interrupting agent");

    // 打断成立，撤掉 duck（若正在等待门槛）与相关状态
    barge_in_pending_.store(false);
    audio_router_.set_output_gain(1.0f);

    // R8：打断响应延迟 —— 从用户开口到停声完成。
    // 实施计划里的硬指标是 <250ms，这里是可观测的实测值。
    turn_interrupted_.store(true);
    trace_event_("USER_BARGE_IN");
    metrics_.record(MetricKind::BargeInLatency, now_ms() - barge_in_start_ms_);
    {
        // 恢复中性，供下一轮使用
        std::lock_guard<std::mutex> lock(policy_mutex_);
        last_policy_action_.clear();
    }

    // ---- R9a：必须在 stop_playback() 之前取播放进度 ----
    // stop_playback() 会清空缓冲，之后就取不到"用户听到了多少"了。
    if (config_.enable_interrupt_truncation) {
        sync_interrupted_context_();
    }

    // ---- R1：按 policy 分派，而不是一刀切全 cancel ----
    // 前台回答（正在播报的内容）立刻停；后台搜索/记忆/预取按各自 policy
    // 决定 cancel / pause / continue / supersede。换话题时用户往往只是
    // 改主意了，把已经跑了一半的搜索杀掉是浪费 —— 跑完的结果还能入 cache。
    auto affected = tasks_.apply_interrupt();
    if (!affected.empty()) {
        LOG_INFO("Interrupt policy applied to {} task(s)", affected.size());
    }

    // 前台轮次仍由 session_token_ 统一收口：LLM 停止生成、TTS 停止合成、
    // 清空待播音频。这三步是"停声"必需的，不走 TaskManager。
    if (session_token_) session_token_->cancel();

    if (llm_) llm_->stop();
    // 先停合成，再淡出播放：tts_->stop() 之后不再有新帧，
    // request_fade_out_stop() 把已缓冲的尾巴在 fade_out_ms 内淡到 0。
    // （顺序反了会边淡边补；AudioRouter 也会直接丢弃淡出期间的新帧兜底。）
    if (tts_) tts_->stop();
    audio_router_.request_fade_out_stop();

    // R6：丢弃还没播出去的段，避免打断后残留半句。
    // 正在播的那段会被 stop() 掐断，清掉整份计划最干净 ——
    // 本轮内容已经作废，不需要保留任何可修订的段。
    response_plan_.clear();
    speech_buffer_pending_.clear();
    synth_cursor_.store(0);
    speech_started_.store(false);
    fast_response_.on_interrupt();   // R3：打断后允许下一轮重新抢答

    smart_turn_.agent_stop_speaking();
    eou_detector_.reset();

    // 前台回答被取代：正在播的这条回答不再继续
    {
        Event e{EventType::ResponseSuperseded, 0, "", {}, "barge-in"};
        e.turn_id = current_turn_id_.load();
        global_event_bus().publish(std::move(e));
    }

    enter_interrupting_();
}

// ========== 状态转移 ==========

void Orchestrator::set_state_(State new_state) {
    State old_state = state_.exchange(new_state);
    LOG_INFO("State: {} → {}", state_to_string(old_state), state_to_string(new_state));
    if (state_cb_) {
        state_cb_(std::string(state_to_string(new_state)));
    }
    global_event_bus().publish(Event{EventType::StateChanged, 0,
        std::string(state_to_string(new_state)), {}, ""});
}

void Orchestrator::enter_idle_() {
    set_state_(State::Idle);
    accumulated_text_.clear();
    eou_detector_.reset();
    smart_turn_.reset();
}

void Orchestrator::enter_listening_() {
    set_state_(State::Listening);
    accumulated_text_.clear();
    eou_detector_.reset();
}

void Orchestrator::enter_eou_pending_(uint64_t timestamp_us) {
    set_state_(State::EouPending);
    (void)timestamp_us;
    // EOUDetector 已在 on_vad_speech_end/on_vad_speech 中管理定时器
}

void Orchestrator::enter_thinking_(const std::string& text) {
    set_state_(State::Thinking);

    // 生成新的 session token
    session_token_ = std::make_shared<CancelToken>();
    tts_accum_ms_.store(0.0);
    tts_accum_active_ = false;
    speech_started_.store(false);   // 新一轮 Thinking：等待首段音频以即时开播

    // R6：新一轮 = 新的响应计划。情绪也复位（上一轮的情绪不该延续到下一轮）。
    response_plan_.clear();
    speech_buffer_pending_.clear();
    synth_cursor_.store(0);
    {
        std::lock_guard<std::mutex> lock(emotion_mutex_);
        current_emotion_.clear();
        current_emotion_intensity_ = 0.0f;
    }

    // R8：起本轮 trace
    turn_start_ns_ = now_ns();
    turn_asr_done_ns_ = turn_start_ns_;   // 进入 thinking 即"文本就绪"
    turn_llm_start_ns_ = now_ns();
    turn_first_token_ns_ = 0;
    turn_first_audio_ns_ = 0;
    turn_interrupted_.store(false);
    turn_user_text_ = text;
    turn_ack_text_.clear();
    turn_events_.clear();
    trace_event_("TURN_START", text);

    // 记下本轮用户输入：LLM 收尾时与回答一起进 ContextManager
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        last_user_text_ = text;
    }

    // ---- R4：按本轮 tier 切换生成参数 ----
    // Policy 在 admit_user_turn_ 里已算出 tier 并存进 last_route_，
    // 这里把它落到 LLM 的采样参数上 —— 这就是"不把所有问题路由到最大模型"。
    {
        RouteResult route;
        {
            std::lock_guard<std::mutex> lock(policy_mutex_);
            route = last_route_;
        }
        ModelTier tier = route.tier;
        if (tier == ModelTier::Search && agent_tools_) {
            // 有工具时用 Agent 档（工具调用需要稳定的 JSON 输出）
            tier = ModelTier::Agent;
        }
        if (tier == ModelTier::Background) {
            tier = ModelTier::Normal;   // 前台轮次不该跑在 Background 档
        }

        RouteResult applied = model_router_.route(tier);
        // R7：判断这一档实际用的是不是远程引擎。降级后落到本地档时
        // 必须为 false，否则会用远程请求去回答一个本该本地快答的问题。
        route_tier_uses_remote_ = (applied.engine.rfind("remote:", 0) == 0);
        if (llm_ && !route_tier_uses_remote_) {
            llm_->apply_sampling(applied.profile.temperature,
                                 applied.profile.top_k,
                                 applied.profile.top_p,
                                 applied.profile.max_tokens);
        }
        {
            std::lock_guard<std::mutex> lock(policy_mutex_);
            last_route_ = applied;
        }
        LOG_INFO("Route: tier=%s engine=%s remote=%s temp=%.2f top_k=%d max_tokens=%d",
                 model_tier_to_string(applied.tier),
                 applied.engine.c_str(),
                 route_tier_uses_remote_ ? "yes" : "no",
                 applied.profile.temperature, applied.profile.top_k,
                 applied.profile.max_tokens);
        {
            Event e{EventType::RouteDecision, 0, "", {},
                    model_tier_to_string(applied.tier)};
            e.turn_id = current_turn_id_.load();
            global_event_bus().publish(std::move(e));
        }
    }

    // ---- R7：远程强模型（DEEP/AGENT/SEARCH 档且已挂远程引擎）----
    // 刻意**不走**本地 AgentLoop 工具链：远程的 tool calling 协议与本地
    // GBNF grammar 是两套系统，混用会产生难以排查的行为差异。远程档只做
    // 对话 + 推理，工具结果通过 ContextManager 的 P6 层喂进去。
    if (remote_ && route_tier_uses_remote_) {
        const double t_remote = now_ms();
        RemoteLLM::ChatRequest creq;

        // system：把分层上下文作为系统消息
        const std::string sys = build_system_prompt_with_context_(text);
        if (!sys.empty()) {
            RemoteMessage m;
            m.role = "system";
            m.content = sys;
            creq.messages.push_back(std::move(m));
        }
        // 最近几轮对话作为历史（ContextManager 的 P3 层已经在 sys 里，
        // 这里只带最近两轮原文，避免重复占用 context）
        for (const auto& t : context_.turns()) {
            if (!t.user.empty()) {
                RemoteMessage m;
                m.role = "user";
                m.content = t.user;
                creq.messages.push_back(std::move(m));
            }
            if (!t.assistant.empty()) {
                RemoteMessage m;
                m.role = "assistant";
                m.content = t.assistant;
                creq.messages.push_back(std::move(m));
            }
        }
        RemoteMessage um;
        um.role = "user";
        um.content = text;
        creq.messages.push_back(std::move(um));

        auto rres = remote_->chat(
            creq,
            [this](const std::string& tok) {
                // 与本地路径同款状态门（见 on_llm_token_ 内注释）：
                // 只认 Thinking 会在首段音频切到 Speaking 后丢掉
                // 后续所有 token —— 远程路径连文字展示都会断流。
                const State st = state_.load();
                if (st == State::Thinking || st == State::Speaking) {
                    on_llm_token_(LLMResponse{tok, false, 0});
                }
            },
            session_token_);

        if (trace_cb_) trace_cb_("llm_remote", now_ms() - t_remote);

        if (rres.cancelled) {
            LOG_INFO("Remote turn cancelled");
            return;
        }
        if (!rres.ok) {
            // 远程失败：降级回本地继续答，而不是让用户等在沉默里。
            // 这是"优雅降级"链的最后一环（见架构方案第 30 节）。
            LOG_WARN("Remote LLM failed ({}), falling back to local", rres.error);
            if (trace_cb_) trace_cb_("llm_remote_failed", 0);
        } else {
            if (!rres.content.empty() || !current_text().empty()) {
                on_llm_complete_();
            } else {
                enter_idle_();
            }
            return;
        }
        // 落到下面走本地
    }

    // ---- /memory 命令：直接执行并播报，不走 LLM ----
    if (memory_ && is_memory_command(text)) {
        std::string reply = memory_->run_command(text);
        LOG_INFO("Memory command -> '{}'", reply);
        on_llm_token_(LLMResponse{reply, false, 0});
        on_llm_complete_();
        return;
    }

    // ---- Agent 工具循环（M5）：挂载后优先使用 ----
    if (agent_tools_ && llm_) {
        ToolExecutor executor(*agent_tools_);
        AgentLoop loop(*llm_, *agent_tools_, executor);

        // 转发阶段计时与工具事件给上层（GUI 时间轴 / 工具区）
        loop.set_trace([this](const std::string& stage, double ms) {
            if (trace_cb_) trace_cb_(stage, ms);
        });
        loop.set_tool_report([this](const std::string& line) {
            if (tool_cb_) tool_cb_(line);
        });

        // R1：system prompt 走 ContextManager 分层拼装
        // （P1 话题 / P3 最近对话 / P4 工作上下文 / P5 召回记忆），
        // 不再在这里手拼 "[相关记忆]"。
        loop.set_system_prompt(build_system_prompt_with_context_(text));
        // 工具意图门控：仅当用户明确要求（搜索/记忆/时间）时才放行对应工具
        loop.set_enabled_tools(detect_tool_intent(text));
        loop.set_token_sink([this](const std::string& token) {
            // 复用原有流式逻辑：文本 token 累积 + 触发流式 TTS
            on_llm_token_(LLMResponse{token, false, 0});
        });

        auto res = loop.run(text, session_token_);

        // 注意：不再自动 ingest 用户事实。记忆写入只发生在用户明确指示时
        // （/memory 命令，或模型在用户要求"记住…"时调用 memory_save 工具）。

        if (res.cancelled) {
            // 打断由 on_barge_in_detected 负责状态迁移（->Interrupting->Listening）
            LOG_INFO("Agent turn cancelled");
            return;
        }

        // 流式 token 已通过 sink 累积到 accumulated_text_，这里统一收尾
        if (!res.final_text.empty() || !current_text().empty()) {
            on_llm_complete_();
        } else {
            enter_idle_();
        }
        return;
    }

    if (llm_) {
        llm_->set_cancel_token(session_token_);

        llm_->generate_stream(text,
            [this](const LLMResponse& chunk) {
                on_llm_token_(chunk);
            });
    } else {
        // 没有 LLM，直接合成占位文本
        on_llm_complete_();
    }
}

void Orchestrator::enter_speaking_() {
    set_state_(State::Speaking);
    smart_turn_.agent_start_speaking();
    audio_router_.start_playback();
}

void Orchestrator::enter_interrupting_() {
    set_state_(State::Interrupting);
    // 瞬时状态，下一帧切回 Listening
    enter_listening_();
}

// ========== EOU 回调 ==========

// ========== R1：Conversation Runtime ==========

std::string Orchestrator::last_policy_reason() const {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    return last_policy_reason_;
}

void Orchestrator::submit_user_text(std::string text) {
    if (!running_.load()) return;
    if (text.empty()) return;

    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        accumulated_text_.clear();
    }
    LOG_INFO("[Orch] PTT 轮提交用户文本 len={} turn={}", text.size(),
             current_turn_id_.load());
    if (user_text_cb_) user_text_cb_(text);

    if (!admit_user_turn_(text)) {
        // Policy 判定保持安静：不占用前台，不打断当前播报
        enter_listening_();
        return;
    }
    enter_thinking_(text);
}

TaskId Orchestrator::submit_background_task(
    std::string name, std::string topic_key,
    std::function<TaskOutcome(TaskContext&)> work) {
    TaskSpec spec;
    spec.name = std::move(name);
    spec.topic_key = std::move(topic_key);
    spec.priority = TaskPriority::Background;
    // 关键：被打断时继续跑完（结果入 cache），不 kill。
    // 用户换主意不代表已经跑到一半的搜索该被扔掉。
    spec.policy = config_.background_continue_on_interrupt
                      ? TaskPolicy::ContinueOnInterrupt
                      : TaskPolicy::CancelOnInterrupt;
    spec.turn_id = current_turn_id_.load();
    spec.work = std::move(work);

    const TaskId id = tasks_.submit(std::move(spec));
    LOG_DEBUG("Background task #{} submitted: {}", id, name);
    return id;
}

void Orchestrator::absorb_background_result_(TaskId id, const TaskOutcome& outcome) {
    if (!outcome.ok || outcome.value.empty()) return;

    auto snap = tasks_.get(id);
    if (!snap.has_value()) return;

    // 喂自适应延迟估计：下次同类任务就知道该不该先说一句
    fast_response_.record_latency(snap->name, static_cast<int>(outcome.elapsed_ms));

    // 关键：should_speak=false（被 supersede / 打断后继续跑完）的结果
    // 只能进 cache，绝不能主动播报抢话。
    bg_results_.put(snap->topic_key.empty() ? ("task:" + std::to_string(id))
                                            : snap->topic_key,
                    outcome.value, outcome.elapsed_ms,
                    /*superseded=*/!outcome.should_speak);

    LOG_INFO("Background task #{} done: {} chars, {}ms, speakable={} -> cache",
             id, outcome.value.size(), static_cast<int>(outcome.elapsed_ms),
             outcome.should_speak);
}

TaskId Orchestrator::spawn_background_for_decision_(const ResponseDecision& decision,
                                                     const std::string& user_text) {
    // 只有 Policy 明确要求、且允许后台化时才派生。
    // 简单问题（Fast tier）绝不派生后台任务 —— 那只是浪费。
    if (!decision.allow_background) return kInvalidTaskId;

    if (decision.needs_search && search_router_) {
        // 搜索：结果要的是结构化 hits 的摘要，不是原始 JSON
        auto router = search_router_;
        const std::string topic = "search:" + user_text.substr(0, 24);
        return submit_background_task("search", topic,
            [router, user_text](TaskContext& ctx) -> TaskOutcome {
                SearchQuery q;
                q.q = user_text;
                q.topk = 8;
                q.lang = "zh-CN";
                q.time_range = "month";   // 时效性查询默认只要最近一月

                auto hits = router->query(q, SearchPolicy::LocalFirst, ctx.token());
                TaskOutcome o;
                if (hits.empty()) {
                    o.ok = false;
                    o.error = "no hits";
                    return o;
                }
                o.ok = true;
                // 拼成可读的紧凑摘要（比原始 hits 省 token）
                std::string s;
                for (size_t i = 0; i < hits.size() && i < 5; ++i) {
                    s += "- " + hits[i].title;
                    if (!hits[i].snippet.empty()) s += "：" + hits[i].snippet;
                    s += "\n";
                }
                o.value = std::move(s);
                // 后台搜索结果默认不主动播报：用户可能已经换话题了。
                // 由上层在确认仍relevant时才播。
                o.should_speak = false;
                return o;
            });
    }

    if (decision.needs_memory && memory_ && memory_->is_open()) {
        // 记忆预取：把召回结果备好，下一轮直接用
        auto mem = memory_;
        const std::string topic = "memory:" + user_text.substr(0, 24);
        return submit_background_task("memory", topic,
            [mem, user_text](TaskContext& ctx) -> TaskOutcome {
                auto recalled = mem->recall(user_text, 5);
                TaskOutcome o;
                if (recalled.empty()) {
                    o.ok = false;
                    o.error = "no memory";
                    return o;
                }
                o.ok = true;
                std::string s;
                for (auto& m : recalled) s += "- " + m.content + "\n";
                o.value = std::move(s);
                o.should_speak = false;
                return o;
            });
    }

    return kInvalidTaskId;
}

void Orchestrator::speak_ack_(const std::string& text) {
    if (text.empty()) return;
    LOG_INFO("[Orch] 抢答 ack '{}'", text);
    // 抢答是"先垫一句"，不进对话历史、不进 LLM 上下文 —— 只是让用户
    // 知道系统接住了。真正的回答还在后面。
    if (tts_ && tts_enabled_.load()) {
        tts_->synthesize_stream(text,
            [this](const int16_t* audio, size_t frames, bool is_last) {
                on_tts_chunk_(audio, frames, is_last);
            });
    }
    if (token_cb_) token_cb_(text);   // GUI 也显示这一句
}

void Orchestrator::inject_cached_results_(const std::string& user_text) {
    // 用户提到"刚才/那个/之前"时，把相关后台结果捞出来给本轮用。
    // 这补上了 R1 的缺口：结果存了但没人读。
    if (user_text.empty()) return;

    auto hit = bg_results_.find_by_keyword(user_text);
    if (!hit.has_value()) return;

    // 追加到 P6 层（与本轮真实工具结果并存，不覆盖）
    context_.add_background_result("[早前后台结果 · " + hit->topic_key + "]\n" +
                                   hit->content);
    LOG_INFO("Injected cached background result: topic='{}' ({} chars)",
             hit->topic_key, hit->content.size());
}

bool Orchestrator::admit_user_turn_(const std::string& text) {
    // 决策全部交给 admission_（唯一决策源），这里只执行它的结论。
    admission_.set_agent_speaking(smart_turn_.is_agent_speaking());
    TurnAdmissionResult r = admission_.evaluate(text);

    {
        std::lock_guard<std::mutex> lock(policy_mutex_);
        last_policy_reason_ = r.reason;
        last_policy_action_ = response_action_to_string(r.decision.action);
        // R4：把 Policy 选中的 tier 记下来，enter_thinking_ 会据此切采样参数
        last_route_.requested = r.decision.tier;
        last_route_.tier = r.decision.tier;
        last_route_.profile = model_router_.profile_for(r.decision.tier);
    }
    LOG_INFO("Admission: intent={} action={} tier={} start_turn={} interrupt={} reason={}",
             user_speech_intent_to_string(r.intent),
             response_action_to_string(r.decision.action),
             model_tier_to_string(r.decision.tier),
             r.should_start_turn, r.should_interrupt, r.reason);

    // 换话题：把上一话题的后台任务标记 superseded（不 kill），
    // 跑完的结果仍进 cache，但不会主动播报抢话。
    if (r.supersede_stale_tasks) {
        for (const auto& snap : tasks_.active()) {
            if (snap.topic_key.empty()) continue;
            if (snap.priority == TaskPriority::Foreground) continue;
            tasks_.supersede_topic(snap.topic_key, kInvalidTaskId);
        }
    }

    // 先让出话轮（用户说了"等一下"/"算了"），再决定要不要起新轮次。
    if (r.should_interrupt && smart_turn_.is_agent_speaking()) {
        interrupt_agent_();
    }

    if (r.should_start_turn) {
        current_turn_id_.store(admission_.current_turn_id());
        // P6 层是"本轮"上下文：新轮次开始先清上一轮的结果条目，
        // 否则会不断累积。内容本身在 BackgroundCache 里，不会丢。
        context_.clear_results();
        Event e{EventType::TurnComplete, 0, text, {}, ""};
        e.turn_id = current_turn_id_.load();
        global_event_bus().publish(std::move(e));

        // ---- R3：Fast Response + Background Agent ----
        fast_response_.begin_turn();

        // 1) 慢任务先垫一句（"我看一下。"），不让用户干等
        const char* task_type = r.decision.needs_search  ? "search"
                                : r.decision.needs_memory ? "memory"
                                : r.decision.needs_agent  ? "agent"
                                                           : "llm";
        FastResponsePlan plan = fast_response_.plan(r.decision, task_type, text);
        if (plan.decision == AckDecision::Acknowledge) {
            speak_ack_(plan.text);
            fast_response_.notify_spoke();
            turn_ack_text_ = plan.text;   // R8：记入 trace
            {
                Event e{EventType::QuickResponse, 0, plan.text, {}, plan.reason};
                e.turn_id = current_turn_id_.load();
                global_event_bus().publish(std::move(e));
            }
            LOG_INFO("Fast response ack: '{}' ({})", plan.text, plan.reason);
        }

        // 2) Policy 说要 search/memory/agent → 把耗时工作挪到后台，
        //    前台不等它。
        const TaskId bg = spawn_background_for_decision_(r.decision, text);
        if (bg != kInvalidTaskId) {
            LOG_INFO("Spawned background task #{} for this turn", bg);
        }

        // 3) 把早前后台结果注入本轮上下文（"刚才那个搜索结果"能被引用）
        inject_cached_results_(text);
    }
    return r.should_start_turn;
}

std::string Orchestrator::build_system_prompt_with_context_(
    const std::string& user_text) {
    std::string sys_prompt = agent_system_prompt_;

    // 召回记忆交给 ContextManager 统一管（P5 层），不再直接往 system prompt 拼。
    if (memory_ && memory_->is_open()) {
        auto recalled = memory_->recall(user_text, 3);
        if (!recalled.empty()) {
            std::vector<std::string> v;
            v.reserve(recalled.size());
            for (auto& m : recalled) v.push_back(m.content);
            context_.set_relevant_memory(std::move(v));
        }
    }

    // WorkingContext 与 ContextManager 共享同一份 WorkingContext 实例，
    // 避免"准入时清了一份、拼 prompt 时读的是另一份"。
    std::string ctx = context_.build_context();
    if (!ctx.empty()) {
        sys_prompt += "\n\n" + ctx + "\n";
    }
    return sys_prompt;
}

// ========== EOU 回调 ==========

void Orchestrator::on_eou_(bool is_eou) {
    if (!running_.load()) return;

    if (is_eou) {
        LOG_INFO("EOU detected, processing transcription");

        std::string text;
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            text = std::move(accumulated_text_);
            accumulated_text_.clear();
        }

        // ---- R9b：语义复核（对标 semantic_vad）----
        // EOUDetector 只有时长阈值，而时长是代理指标不是目标：
        //   "我想问一下...(停 600ms)...那个项目"  纯时长会抢话
        //   "好的"                                    纯时长会慢半拍
        //
        // 语义判断的职责是"延长等待"而非"缩短"。设计成**只调节 EOU 阈值**：
        // 语义明显没说完时把 force_ms 临时拉长（不改变状态机流程），
        // 用户继续说话则 VAD 会重新触发 on_vad_speech 走正常流程。
        // 这样避免了"已取走文本再放回"这类脆弱的状态回滚。
        if (!text.empty() && config_.enable_semantic_turn) {
            const int waited = eou_detector_.waiting_ms();
            const bool yield = semantic_turn_.should_yield(text, waited);
            auto d = semantic_turn_.last_decision();

            metrics_.record(MetricKind::EouDelay, waited);
            trace_event_("SEMANTIC_TURN", d.reason);

            if (!yield && d.hint == CompletionHint::Incomplete) {
                // 明显没说完 —— 把 force 阈值拉长到语义上限，给用户更多时间。
                const int ext = eagerness_config(semantic_turn_.eagerness()).max_wait_ms;
                if (ext > config_.eou_force_ms) {
                    EOUDetector::Config relaxed = eou_detector_.config();
                    relaxed.force_ms = ext;
                    eou_detector_.set_config(relaxed);
                    LOG_DEBUG("EOU extended to %dms (semantic: incomplete) — %s",
                              ext, d.reason.c_str());
                }
            }
        }

        if (!text.empty()) {
            enter_thinking_(text);
        } else {
            enter_idle_();
        }
    }
}

// ========== ASR 回调 ==========

void Orchestrator::on_asr_result_(const ASRResult& result) {
    if (!running_.load()) return;

    std::lock_guard<std::mutex> lock(text_mutex_);

    if (result.is_final) {
        accumulated_text_ += result.text;
    } else {
        // 部分结果：更新显示，但不触发 EOU
        accumulated_text_ += result.text;
    }
}

// ========== LLM 回调 ==========

void Orchestrator::on_llm_token_(const LLMResponse& chunk) {
    if (!running_.load()) return;

    if (!chunk.text.empty()) {
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            accumulated_text_ += chunk.text;
        }

        // R8：首 token 到达（TTFT 观测点）
        if (turn_first_token_ns_ == 0) {
            turn_first_token_ns_ = now_ns();
            LOG_INFO("[Orch] LLM 首 token len={} turn={}", chunk.text.size(),
                     current_turn_id_.load());
            if (turn_llm_start_ns_ > 0) {
                const double ttft = static_cast<double>(turn_first_token_ns_ -
                                                         turn_llm_start_ns_) / 1e6;
                metrics_.record(MetricKind::LlmTtft, ttft);
                trace_event_("LLM_FIRST_TOKEN");
            }
        }

        // 回调给上层（流式增量 → 实时回复面板）
        if (token_cb_) {
            token_cb_(chunk.text);
        }

        // 流式触发 TTS（边生成边合成）；开关关闭时跳过。
        // R6：文本先进 ResponsePlan，由 ProsodyPlanner 切句 + 加韵律，
        // 再经 ITtsAdapter 落到具体引擎。
        // 仍然坚持"句级"而非逐 token 直灌 —— SAPI 一次 Speak 会清空当前读本，
        // 逐字喂会互相打断只读几字。
        //
        // 状态门必须是 Thinking|Speaking，不能只认 Thinking：
        // 首段音频一到 on_tts_chunk_ 就 enter_speaking_() 切到 Speaking，
        // 而 LLM 此时往往还在继续出 token —— 只认 Thinking 的话，
        // 第一个标点之后的内容全部进不了 TTS（表现为"只读第一句"）。
        // Interrupting/Listening 不喂：打断路径已清空缓冲，迟到的
        // token 不该复活成下一句的残肢。
        {
            const State st = state_.load();
            if (tts_ && tts_enabled_.load() &&
                (st == State::Thinking || st == State::Speaking)) {
                feed_speech_text_(chunk.text);
            }
        }

        // LLM 结束
        if (chunk.eos) {
            on_llm_complete_();
        }
    }
}

void Orchestrator::on_llm_complete_() {
    if (!running_.load()) return;

    // R6：把未到句子边界的残留文本作为尾句补进来
    // （LLM 结束但尾句无标点，也要播出来）
    if (tts_ && tts_enabled_.load()) {
        flush_speech_tail_();
    }

    std::string final_text;
    std::string user_text;
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        final_text = std::move(accumulated_text_);
        user_text = last_user_text_;
    }

    LOG_INFO("LLM generation complete: {} chars", final_text.size());

    // R1：本轮落进 WorkingContext —— 用户输入 + 助手回答。
    // 下一轮拼 prompt 时会作为 P3 层（最近对话）注入，"那个/刚才那个"
    // 才有东西可指。
    if (!final_text.empty()) {
        context_.add_turn(user_text, final_text);
    }

    // R8：本轮 trace 收尾（写盘 + 指标）
    finish_turn_trace_();

    // 完整回答整段回调一次（GUI 以此在对话区落一条完整气泡 + 记录最近回复）
    if (!final_text.empty() && text_callback_) {
        text_callback_(final_text);
    }

    // 上报流式 TTS 累计耗时（若有）
    if (tts_accum_active_) {
        tts_accum_active_ = false;
        const double t = tts_accum_ms_.load();
        if (t > 0.0 && trace_cb_) trace_cb_("tts", t);
        tts_accum_ms_.store(0.0);
    }

    if (!final_text.empty()) {
        // 已在流式播放中（on_tts_chunk_ 触发过 enter_speaking_）：保持 Speaking，
        // 不要再次 enter_speaking_——那会重跑 start_playback() 清空已缓冲的音频。
        // TTS 关闭或无流式音频时才据此进入 Speaking（空结束 → 兜底一次）。
        if (!speech_started_.load()) {
            enter_speaking_();
        }
    } else {
        enter_idle_();
    }
}

// ========== TTS 回调 ==========

void Orchestrator::on_tts_chunk_(const int16_t* audio, size_t frames, bool is_last) {
    if (!running_.load()) return;

    // 流式播放：Thinking 阶段首段音频一到，立即进入 Speaking 并开始播放，
    // 不必等 LLM 全部生成完（否则 TTS 音频会憋在 AudioRouter 里直到结束才出声）。
    if (state_.load() == State::Thinking && !speech_started_.exchange(true)) {
        enter_speaking_();
        // R8：首段音频 —— 这是体感最关键的一条指标
        turn_first_audio_ns_ = now_ns();
        if (turn_asr_done_ns_ > 0) {
            metrics_.record(MetricKind::AsrToFirstAudio,
                            static_cast<double>(turn_first_audio_ns_ -
                                                turn_asr_done_ns_) / 1e6);
        }
        trace_event_("TTS_FIRST_AUDIO");
    }

    if (audio_router_.is_playing()) {
        audio_router_.push_tts_frames(audio, frames);
    }

    if (is_last) {
        LOG_DEBUG("TTS stream complete");
        // R6：段合成完成，推进 ResponsePlan 的播放游标
        const size_t idx = synth_cursor_.load();
        response_plan_.mark_played(idx);
        synth_cursor_.store(idx + 1);
        // 还有下一段就继续送，形成连播
        if (response_plan_.pending_count() > 0) {
            pump_next_segment_();
        }
    }
}

// ========== R6：韵律 / 响应修订 ==========

void Orchestrator::set_emotion(std::string emotion, float intensity) {
    std::lock_guard<std::mutex> lock(emotion_mutex_);
    current_emotion_ = std::move(emotion);
    // 强度不足直接清空 —— 半吊子的情绪比没有情绪更像机器人
    current_emotion_intensity_ = intensity;
}

void Orchestrator::feed_speech_text_(const std::string& token) {
    if (token.empty()) return;

    // 累积缓冲放在 ResponsePlan 之外：ResponsePlan 管的是"已切好的段"，
    // 这里管的是"还没到句子边界的半句"。
    speech_buffer_pending_ += token;

    // 切出所有完整句。hold_tail=true：无终止标点的半句留在缓冲里等
    // 下一批 token 拼完整 —— 不 hold 的话每个 token 的尾巴都会被当成
    // "整句"立刻送去合成，一句话被拆成碎片，且首段极短、音频来得
    // 极快，会立刻把状态机切到 Speaking。
    auto sents = ProsodyPlanner::split_sentences(speech_buffer_pending_, 120,
                                                 /*hold_tail=*/true);
    if (sents.empty()) return;

    // 返回的各段即"已消费"部分（含被丢弃的纯标点段之外的完整句），
    // 半句尾巴留在缓冲里等下一批 token
    size_t consumed = 0;
    for (const auto& s : sents) consumed += s.size();
    speech_buffer_pending_.erase(0, consumed);

    // 保留纯空白（split 会把它们丢掉）
    const size_t first_nonspace = speech_buffer_pending_.find_first_not_of(" \t\r\n");
    if (first_nonspace == std::string::npos) {
        speech_buffer_pending_.clear();
    } else if (first_nonspace > 0) {
        speech_buffer_pending_.erase(0, first_nonspace);
    }

    std::string emotion;
    float intensity = 0.0f;
    {
        std::lock_guard<std::mutex> lock(emotion_mutex_);
        emotion = current_emotion_;
        intensity = current_emotion_intensity_;
    }

    // 逐段 plan（不整体再切一次 —— sents 已经是切好的句子）
    std::vector<SpeechSegment> segs;
    segs.reserve(sents.size());
    for (size_t i = 0; i < sents.size(); ++i) {
        auto seg = prosody_planner_.plan_segment(sents[i], emotion, intensity);
        if (i == 0 && synth_cursor_.load() == 0) {
            seg.pause_before_ms = 60;   // 轮次开头稍停
        }
        segs.push_back(std::move(seg));
    }

    response_plan_.append(segs);
    tts_accum_active_ = true;
    pump_next_segment_();
}

void Orchestrator::flush_speech_tail_() {
    if (speech_buffer_pending_.empty()) return;

    std::string tail = std::move(speech_buffer_pending_);
    speech_buffer_pending_.clear();
    if (!sentence_has_content(tail)) return;

    std::string emotion;
    float intensity = 0.0f;
    {
        std::lock_guard<std::mutex> lock(emotion_mutex_);
        emotion = current_emotion_;
        intensity = current_emotion_intensity_;
    }
    auto seg = prosody_planner_.plan_segment(tail, emotion, intensity);
    // 轮次末尾留白：否则下一轮的第一句紧贴上来，像说了两件事
    seg.pause_after_ms = std::max(seg.pause_after_ms, 180);
    response_plan_.append({std::move(seg)});
    tts_accum_active_ = true;
    pump_next_segment_();
}

bool Orchestrator::pump_next_segment_() {
    if (!tts_ || !tts_adapter_) return false;

    // claim_next 在锁内原子完成"取段+占用"。pump 会从 LLM 线程
    // （feed）和 TTS 回调线程（is_last 连播）并发进来，不原子的话
    // 两边会拿到同一段，重复合成出重叠音频。
    SpeechSegment seg;
    const size_t idx = response_plan_.claim_next(seg);
    if (idx == ResponsePlan::npos) return false;

    synth_cursor_.store(idx);   // on_tts_chunk_ 的 is_last 按它 mark_played

    tts_->set_cancel_token(session_token_);
    const double t0 = now_ms();
    tts_adapter_->synthesize(
        seg, [this](const int16_t* audio, size_t frames, bool is_last) {
            on_tts_chunk_(audio, frames, is_last);
        });
    tts_accum_ms_.store(tts_accum_ms_.load() + (now_ms() - t0));
    LOG_DEBUG("Synthesized segment #{} ({} chars, {}ms est): {}",
              idx, seg.text.size(), seg.estimated_ms, seg.text);
    return true;
}

size_t Orchestrator::discard_unplayed() {
    const size_t n = response_plan_.discard_all_unplayed();
    if (n > 0) {
        speech_buffer_pending_.clear();
        LOG_INFO("Discarded {} unplayed segment(s) (response revision)", n);
    }
    return n;
}

// ========== R9a：打断上下文同步 ==========

void Orchestrator::sync_interrupted_context_() {
    // 关键前提：必须在 stop_playback() 之前调用。
    // stop_playback() 会清空 tts_buffer_，之后 played_ratio 恒为 0。
    const double ratio = audio_router_.played_ratio();
    const int64_t pushed = audio_router_.total_frames_pushed();

    // 本轮模型生成了多少（可能远多于播出的）
    std::string generated;
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        generated = accumulated_text_;
    }
    if (generated.empty() && pushed == 0) {
        return;   // 还没来得及说任何话 —— 不算截断
    }

    TruncationResult r = truncator_.truncate(generated, ratio);
    if (!r.truncated) return;

    LOG_INFO("Interrupt truncation: played %.0f%% (%d chars kept, %d dropped)",
             ratio * 100.0, static_cast<int>(r.played_text.size()),
             static_cast<int>(r.dropped_text.size()));

    // 1) 把"用户实际听到的"记入对话历史。
    //    这样下一轮模型知道用户确实听过这部分内容。
    if (!r.played_text.empty()) {
        context_.add_turn(turn_user_text_, r.played_text);
    }

    // 2) 构造"被打断"说明写进 P2 层，下一轮 prompt 能看到。
    //    对应 Azure 的 appended_text_after_truncation —— 模型若不知道
    //    自己被打断，下一轮会困惑于"为什么我话没说完"。
    pending_interrupt_note_ = truncator_.build_interrupt_note(r);
    if (!pending_interrupt_note_.empty()) {
        context_.set_task_state_summary(pending_interrupt_note_);
    }

    // 3) trace：截断比例是判断"播报是否太啰嗦"的关键信号 ——
    //    大量截断说明回答比用户耐心长。
    trace_event_("RESPONSE_TRUNCATED", r.played_text);
}

// ========== R8：可观测 ==========

void Orchestrator::trace_event_(const std::string& type, const std::string& detail) {
    if (!trace_enabled()) return;
    TraceEvent ev;
    ev.t_us = turn_start_ns_ > 0
                  ? static_cast<uint64_t>((now_ns() - turn_start_ns_) / 1000)
                  : 0;
    ev.type = type;
    ev.detail = detail;
    turn_events_.push_back(std::move(ev));
}

void Orchestrator::finish_turn_trace_() {
    if (!trace_enabled()) return;

    // 补几个没打到的时点
    if (turn_asr_done_ns_ == 0) turn_asr_done_ns_ = turn_llm_start_ns_;

    trace_event_("TURN_COMPLETE");

    TurnRecord rec;
    rec.turn_id = std::to_string(current_turn_id_.load());
    rec.user_text = turn_user_text_;
    rec.user_intent = user_speech_intent_to_string(last_user_intent());
    {
        std::lock_guard<std::mutex> lock(policy_mutex_);
        rec.policy_action = last_policy_action_;
        rec.policy_reason = last_policy_reason_;
        rec.route_tier = model_tier_to_string(last_route_.tier);
        rec.route_engine = last_route_.engine;
    }
    rec.spoke_ack = !turn_ack_text_.empty();
    rec.ack_text = turn_ack_text_;
    rec.interrupted = turn_interrupted_.load();
    rec.discard_count = static_cast<int>(response_plan_.stats().discarded);

    const int64_t now = now_ns();
    if (turn_asr_done_ns_ > 0 && turn_start_ns_ > 0) {
        rec.speech_to_asr_ms =
            static_cast<double>(turn_asr_done_ns_ - turn_start_ns_) / 1e6;
    }
    if (turn_first_token_ns_ > 0 && turn_llm_start_ns_ > 0) {
        rec.llm_ttft_ms =
            static_cast<double>(turn_first_token_ns_ - turn_llm_start_ns_) / 1e6;
    }
    if (turn_first_audio_ns_ > 0 && turn_asr_done_ns_ > 0) {
        rec.asr_to_first_audio_ms =
            static_cast<double>(turn_first_audio_ns_ - turn_asr_done_ns_) / 1e6;
    }
    if (turn_start_ns_ > 0) {
        rec.total_ms = static_cast<double>(now - turn_start_ns_) / 1e6;
        metrics_.record(MetricKind::TurnTotal, rec.total_ms);
    }
    rec.events = turn_events_;

    trace_writer_->write_turn(rec);
}

// ========== 辅助 ==========

std::string Orchestrator::current_text() const {
    std::lock_guard<std::mutex> lock(text_mutex_);
    return accumulated_text_;
}

void Orchestrator::set_text_callback(OrchestratorCallback cb) {
    text_callback_ = std::move(cb);
}

void Orchestrator::set_tts_enabled(bool enabled) {
    const bool was = tts_enabled_.exchange(enabled);
    if (was && !enabled) {
        // 关闭播报：中止正在进行的合成与播放
        if (tts_) tts_->stop();
        audio_router_.stop_playback();
        // R6：清掉响应计划 —— 关闭后再打开不应该把旧内容接着念
        response_plan_.clear();
        speech_buffer_pending_.clear();
        synth_cursor_.store(0);
        LOG_INFO("TTS playback disabled");
    } else if (!was && enabled) {
        LOG_INFO("TTS playback enabled");
    }
}

// ========== Agent 工具集成（M5）==========

void Orchestrator::attach_agent(std::shared_ptr<ToolRegistry> registry,
                                std::shared_ptr<SearchRouter> search,
                                std::string system_prompt) {
    agent_tools_ = std::move(registry);
    search_router_ = std::move(search);
    agent_system_prompt_ = std::move(system_prompt);
    LOG_INFO("Agent attached ({} tools, search={})",
             agent_tools_ ? agent_tools_->names().size() : 0u,
             search_router_ ? "yes" : "no");
}

void Orchestrator::attach_memory(std::shared_ptr<MemoryManager> memory) {
    memory_ = std::move(memory);
    LOG_INFO("Memory attached: {}", memory_ ? "yes" : "no");
}

// ========== 管道操作（预留）==========

void Orchestrator::start_capture_() {
    // 采集由 AudioPipeline 统一管理，此处预留
}

void Orchestrator::stop_capture_() {
    // 采集由 AudioPipeline 统一管理，此处预留
}

}  // namespace voice_agent

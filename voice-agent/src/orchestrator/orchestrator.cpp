// src/orchestrator/orchestrator.cpp
#include "orchestrator.hpp"
#include "util/log.hpp"
#include <cstring>
#include <chrono>

namespace voice_agent {

namespace {
inline double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
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
{
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

    // AudioRouter 淡出回调
    audio_router_.set_fade_out_callback([this]() {
        on_barge_in_detected();
    });
    audio_router_.set_fade_out_ms(config_.fade_out_ms);

    LOG_INFO("Orchestrator initialized");
    return true;
}

// ========== 生命周期 ==========

void Orchestrator::start() {
    if (running_.exchange(true)) return;

    session_token_ = std::make_shared<CancelToken>();

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
        });

        // 注册 AudioPipeline 播放回调（从 AudioRouter 拉取 TTS 音频）
        audio_pipeline_->set_playback_callback([this](int16_t* data, size_t frames) {
            if (audio_router_.is_playing()) {
                if (!audio_router_.get_playback_frames(data, frames)) {
                    // 播放完毕，切回 Idle
                    audio_router_.stop_playback();
                    std::memset(data, 0, frames * sizeof(int16_t));
                    enter_idle_();
                }
            } else {
                std::memset(data, 0, frames * sizeof(int16_t));
            }
        });

        audio_pipeline_->start();
    }

    enter_idle_();
    LOG_INFO("Orchestrator started");
}

void Orchestrator::stop() {
    if (!running_.exchange(false)) return;

    if (session_token_) {
        session_token_->cancel();
        session_token_.reset();
    }

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

    // 如果 Agent 正在说话，评估打断意图
    if (s == State::Speaking && smart_turn_.is_agent_speaking()) {
        // 估算用户连续说话时长（简化：假设从用户开始说话到现在）
        auto decision = smart_turn_.evaluate_barge_in(
            static_cast<int>((timestamp_us - smart_turn_.is_agent_speaking() ? 0 : 0) / 1000), 0.0f);
        if (decision == TurnDecision::AgentYield) {
            enter_interrupting_();
        }
    }
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
        if (user_text_cb_ && !text.empty()) user_text_cb_(text);
        if (text.empty()) {
            enter_idle_();
        } else {
            enter_thinking_(text);
        }
        turn_busy_ = false;
    }).detach();
}

void Orchestrator::on_barge_in_detected() {
    if (!running_.load()) return;

    LOG_WARN("Barge-in detected, interrupting agent");

    // 取消当前 LLM 和 TTS
    if (session_token_) session_token_->cancel();

    if (llm_) llm_->stop();
    if (tts_) {
        tts_->stop();
        audio_router_.stop_playback();
    }

    smart_turn_.agent_stop_speaking();
    eou_detector_.reset();

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

        // 记忆召回注入 system prompt
        std::string sys_prompt = agent_system_prompt_;
        if (memory_ && memory_->is_open()) {
            auto recalled = memory_->recall(text, 3);
            if (!recalled.empty()) {
                std::string ctx = "\n\n[相关记忆]";
                for (auto& m : recalled) {
                    ctx += "\n- " + m.content;
                }
                ctx += "\n";
                sys_prompt += ctx;
            }
        }
        loop.set_system_prompt(sys_prompt);
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

        // 回调给上层（流式增量 → 实时回复面板；不在此处触发"完整回答"，
        // 避免每个 token 都产生一条 llmComplete 气泡）
        if (token_cb_) {
            token_cb_(chunk.text);
        }

        // 流式触发 TTS（边生成边合成）；开关关闭时跳过
        if (tts_ && tts_enabled_.load() && state_.load() == State::Thinking) {
            tts_->set_cancel_token(session_token_);
            tts_accum_active_ = true;
            const double t0 = now_ms();
            tts_->synthesize_stream(chunk.text,
                [this](const int16_t* audio, size_t frames, bool is_last) {
                    on_tts_chunk_(audio, frames, is_last);
                });
            tts_accum_ms_.store(tts_accum_ms_.load() + (now_ms() - t0));
        }

        // LLM 结束
        if (chunk.eos) {
            on_llm_complete_();
        }
    }
}

void Orchestrator::on_llm_complete_() {
    if (!running_.load()) return;

    std::string final_text;
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        final_text = std::move(accumulated_text_);
    }

    LOG_INFO("LLM generation complete: {} chars", final_text.size());

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
        enter_speaking_();
    } else {
        enter_idle_();
    }
}

// ========== TTS 回调 ==========

void Orchestrator::on_tts_chunk_(const int16_t* audio, size_t frames, bool is_last) {
    if (!running_.load()) return;

    if (audio_router_.is_playing()) {
        audio_router_.push_tts_frames(audio, frames);
    }

    if (is_last) {
        LOG_DEBUG("TTS stream complete");
    }
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

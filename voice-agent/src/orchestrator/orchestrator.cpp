// src/orchestrator/orchestrator.cpp
#include "orchestrator.hpp"
#include "util/log.hpp"
#include <cstring>

namespace voice_agent {

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

    // 注册 AudioPipeline 输入回调（采集数据流向 VAD）
    if (audio_pipeline_) {
        audio_pipeline_->set_input_callback([this](const int16_t* data, size_t frames) {
            if (vad_) {
                // 下采样 48kHz → 16kHz
                std::vector<int16_t> pcm_16k(frames / 3);
                for (size_t i = 0; i < pcm_16k.size(); ++i) {
                    int32_t sum = 0;
                    for (int j = 0; j < 3; ++j) sum += data[i * 3 + j];
                    pcm_16k[i] = static_cast<int16_t>(sum / 3);
                }
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

    eou_detector_.reset();
    smart_turn_.reset();
    audio_router_.stop_playback();

    LOG_INFO("Orchestrator stopped");
}

// ========== 事件注入 ==========

void Orchestrator::on_vad_speech_start(uint64_t timestamp_us) {
    if (!running_.load()) return;

    State s = state_.load();
    LOG_DEBUG("VAD speech start in state {}", state_to_string(s));

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
    eou_detector_.on_vad_end(timestamp_us);
    smart_turn_.user_speech_end(timestamp_us);
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
        loop.set_token_sink([this](const std::string& token) {
            // 复用原有流式逻辑：文本 token 累积 + 触发流式 TTS
            on_llm_token_(LLMResponse{token, false, 0});
        });

        auto res = loop.run(text, session_token_);

        // 自动 ingest 用户事实
        if (!res.cancelled && memory_ && memory_->is_open()) {
            memory_->ingest(text, res.final_text);
        }

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

        // 回调给上层
        if (text_callback_) {
            text_callback_(chunk.text);
        }

        // 流式触发 TTS（边生成边合成）
        if (tts_ && state_.load() == State::Thinking) {
            tts_->set_cancel_token(session_token_);
            tts_->synthesize_stream(chunk.text,
                [this](const int16_t* audio, size_t frames, bool is_last) {
                    on_tts_chunk_(audio, frames, is_last);
                });
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

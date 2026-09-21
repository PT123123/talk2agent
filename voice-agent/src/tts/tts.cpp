// src/tts/tts.cpp
#include "tts/tts.hpp"
#include "tts/sapi_speaker.hpp"
#include "util/log.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#ifdef USE_SHERPAONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif

#include "util/gpu.hpp"

namespace voice_agent {

// ========== TTS 实现（pimpl） ==========
struct TTS::Impl {
    TTSCallback callback;
    TTSConfig cfg_;

    // 简单引擎：Windows 系统语音（免模型）
    std::shared_ptr<SapiSpeaker> sapi;
    std::function<void()> done_cb;

#ifdef USE_SHERPAONNX
    const SherpaOnnxOfflineTts* tts_{nullptr};
    int sample_rate_{24000};
    std::string provider_;  // 实际生效 provider（directml / cpu）

    ~Impl() { destroy_engine(); }

    void destroy_engine() {
        if (tts_) {
            SherpaOnnxDestroyOfflineTts(tts_);
            tts_ = nullptr;
        }
    }

    // 用指定 provider 重建引擎（provider 传 "dml" / "cpu"）
    bool create_engine(const TTSConfig& config, const std::string& prov) {
        destroy_engine();
        std::string p = (prov == "dml") ? "directml" : "cpu";  // sherpa 认 directml

        SherpaOnnxOfflineTtsConfig c;
        memset(&c, 0, sizeof(c));
        c.model.kokoro.model = config.model_path.c_str();
        if (!config.voice_path.empty())
            c.model.kokoro.voices = config.voice_path.c_str();
        if (!config.tokens_path.empty())
            c.model.kokoro.tokens = config.tokens_path.c_str();
        if (!config.data_dir.empty())
            c.model.kokoro.data_dir = config.data_dir.c_str();
        if (!config.lexicon.empty())
            c.model.kokoro.lexicon = config.lexicon.c_str();
        c.model.kokoro.length_scale = 1.0f / std::max(0.25f, config.speed);
        c.model.num_threads = 2;
        c.model.debug = 0;
        c.model.provider = p.c_str();
        c.max_num_sentences = 2;

        tts_ = SherpaOnnxCreateOfflineTts(&c);
        if (!tts_) {
            LOG_ERROR("KokoroTTS: failed to create engine ({})", config.model_path);
            return false;
        }
        provider_ = p;
        sample_rate_ = SherpaOnnxOfflineTtsSampleRate(tts_);
        LOG_INFO("KokoroTTS: engine ready, sample_rate={}, speakers={}, provider={}",
                 sample_rate_, SherpaOnnxOfflineTtsNumSpeakers(tts_), prov);
        return true;
    }
#endif

    // ---- 简单引擎辅助 ----
    void set_speech_done_callback_impl() {
        if (!sapi) return;
        sapi->set_done_callback([this] {
            auto cb = done_cb;
            if (cb) cb();
        });
    }
    void set_speech_done_callback(std::function<void()> cb) {
        done_cb = std::move(cb);
        if (sapi) {
            sapi->set_done_callback([this] {
                auto cb = done_cb;
                if (cb) cb();
            });
        }
    }
    void speak_simple(const std::string& text) {
        if (sapi) sapi->speak(text);
    }

    bool load(const TTSConfig& config) {
        cfg_ = config;

        // ---- 简单引擎（默认）：Windows 系统语音，零模型、即时、可流式 ----
        if (config.engine == "simple") {
            sapi = std::make_shared<SapiSpeaker>();
            const bool ok = sapi->initialize();
            if (ok) {
                LOG_INFO("SimpleTTS(SAPI): 系统语音就绪，voice={}", sapi->voice_name());
                set_speech_done_callback_impl();
            } else {
                LOG_WARN("SimpleTTS(SAPI): 系统语音不可用（朗读将不发声）");
            }
            return true;
        }

#ifdef USE_SHERPAONNX
        // provider: auto → 运行时支持 DirectML 就用 GPU，否则 CPU
        std::string prov = config.provider;
        if (prov == "auto") prov = ort_dml_available() ? "dml" : "cpu";
        if (prov == "dml" && !ort_dml_available()) {
            LOG_WARN("KokoroTTS: provider=dml 但运行时无 DirectML 支持，回退 cpu");
            prov = "cpu";
        }
        // Intel 显卡 + DirectML 对 Kokoro 的 grouped ConvTranspose 会直接崩溃（0xC0000409），
        // 无法运行时捕获，只能在启动时规避：Intel 适配器默认走 CPU。
        if (prov == "dml" && ort_dml_intel_adapter()) {
            LOG_WARN("KokoroTTS: 检测到 Intel 显卡，DirectML 对 Kokoro 存在已知崩溃，回退 cpu");
            prov = "cpu";
        }
        return create_engine(config, prov);
#else
        LOG_INFO("MockTTS: initialized ({})", config.model_path);
        return true;
#endif
    }

    std::vector<int16_t> synthesize(const TTSConfig& config, const std::string& text) {
        // 简单引擎：直接调用系统语音朗读（异步、即时出声），不返回采样
        if (sapi) {
            speak_simple(text);
            return {};
        }
#ifdef USE_SHERPAONNX
        if (!tts_) return {};

        auto generate = [&]() -> const SherpaOnnxGeneratedAudio* {
            SherpaOnnxGenerationConfig g;
            memset(&g, 0, sizeof(g));
            g.sid = config.speaker_id;
            g.speed = std::max(0.25f, config.speed);
            g.silence_scale = 0.2f;
            return SherpaOnnxOfflineTtsGenerateWithConfig(tts_, text.c_str(), &g,
                                                          nullptr, nullptr);
        };

        const SherpaOnnxGeneratedAudio* audio = generate();
        // Intel Arc + DirectML 对 Kokoro 的 grouped ConvTranspose 不支持（0x80070057），
        // 推理时才会失败；DML 失败则回退 CPU 重建引擎并重试一次。
        if ((!audio || audio->n <= 0) && provider_ == "directml") {
            if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
            LOG_WARN("KokoroTTS: DirectML 推理失败，回退 CPU 重建引擎重试");
            if (!create_engine(cfg_, "cpu")) return {};
            audio = generate();
        }
        if (!audio || audio->n <= 0) {
            LOG_WARN("KokoroTTS: generation failed for '{}'", text);
            if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
            return {};
        }

        std::vector<int16_t> out(audio->n);
        for (int32_t i = 0; i < audio->n; ++i) {
            float s = audio->samples[i];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            out[i] = static_cast<int16_t>(s * 32767.0f);
        }
        LOG_INFO("KokoroTTS: '{}' -> {} samples @ {} Hz ({} chars, provider={})",
                 text, out.size(), audio->sample_rate, text.size(), provider_);
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        return out;
#else
        // Generate 1 second of simple sine wave @ 24kHz
        std::vector<int16_t> audio(24000);
        for (size_t i = 0; i < audio.size(); ++i) {
            float t = static_cast<float>(i) / 24000.0f;
            audio[i] = static_cast<int16_t>(16000 * std::sin(2.0f * 3.14159f * 440.0f * t));  // 440 Hz
        }
        LOG_INFO("MockTTS: synthesized {} chars -> {} samples", text.size(), audio.size());
        return audio;
#endif
    }

    void stop() {
        if (sapi) sapi->stop();
    }
};

// ========== TTS 主类实现 ==========

TTS::TTS() = default;
TTS::~TTS() = default;

bool TTS::initialize(const TTSConfig& config) {
    config_ = config;
    simple_engine_ = (config.engine == "simple");
    impl_ = std::make_unique<Impl>();

    if (!impl_->load(config)) {
        LOG_ERROR("TTS: failed to load model");
        return false;
    }

    LOG_INFO("TTS: engine={} init, speed={}, lang={}, speaker={}",
             config.engine, config.speed, config.lang, config.speaker_id);
    return true;
}

std::vector<int16_t> TTS::synthesize(const std::string& text) {
    if (!impl_) return {};

    synthesizing_ = true;
    const auto t0 = std::chrono::steady_clock::now();
    auto result = impl_->synthesize(config_, text);
    last_synthesize_ms.store(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count());
    synthesizing_ = false;
    return result;
}

void TTS::synthesize_stream(const std::string& text, TTSCallback callback) {
    if (!impl_) return;

    synthesizing_ = true;
    impl_->callback = callback;

    const auto t0 = std::chrono::steady_clock::now();

    // 简单引擎：文本直接交给系统语音实时朗读，随后通知一次"流结束"
    if (simple_engine_) {
        impl_->synthesize(config_, text);
        last_synthesize_ms.store(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0)
                .count());
        synthesizing_ = false;
        if (callback) callback(nullptr, 0, true);
        return;
    }

    auto audio = impl_->synthesize(config_, text);
    size_t chunk_size = 4800;  // 200ms @ 24kHz

    for (size_t i = 0; i < audio.size(); i += chunk_size) {
        size_t end = std::min(i + chunk_size, audio.size());
        bool is_last = (end >= audio.size());

        callback(audio.data() + i, end - i, is_last);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    last_synthesize_ms.store(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count());
    synthesizing_ = false;
}

void TTS::stop() {
    synthesizing_ = false;
    if (impl_) {
        impl_->stop();
    }
}

void TTS::set_cancel_token(std::shared_ptr<CancelToken> token) {
    cancel_token_ = std::move(token);
}

void TTS::set_speech_done_callback(std::function<void()> cb) {
    if (impl_) impl_->set_speech_done_callback(std::move(cb));
}

std::string TTS::simple_voice_name() const {
    if (impl_ && impl_->sapi) return impl_->sapi->voice_name();
    return {};
}

bool TTS::uses_real_backend() const {
    if (simple_engine_) return impl_ && impl_->sapi && impl_->sapi->available();
#ifdef USE_SHERPAONNX
    return impl_ && impl_->tts_ != nullptr;
#else
    return false;
#endif
}

std::string TTS::provider_label() const {
    if (simple_engine_) return "系统语音(SAPI)";
#ifdef USE_SHERPAONNX
    if (!impl_ || !impl_->tts_) return "Mock";
    return impl_->provider_ == "directml" ? "DirectML GPU" : "CPU";
#else
    return "Mock";
#endif
}

// ========== 工厂函数 ==========

std::unique_ptr<TTS> create_kokoro_tts() {
    return std::make_unique<TTS>();
}

}  // namespace voice_agent

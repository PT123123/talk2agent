// src/tts/tts.cpp
#include "tts/tts.hpp"
#include "tts/sapi_speaker.hpp"
#include "tts/tts_bridge.hpp"
#include "util/log.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#ifdef USE_SHERPAONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif

#include "util/gpu.hpp"

#include <fstream>

namespace voice_agent {

// ========== TTS 实现（pimpl） ==========

namespace {

// 引擎名 → 日志前缀。
// 之前这些日志一律硬编码成 "KokoroTTS"，于是 ZipVoice 跑出来的日志写着
// "KokoroTTS: ..." ——排查时会被直接误导（"我明明选的 ZipVoice"）。
const char* engine_display_name(const std::string& engine) {
    if (engine == "kokoro") return "Kokoro";
    if (engine == "piper") return "Piper";
    if (engine == "zipvoice") return "ZipVoice";
    if (engine == "simple") return "Simple";
    return "Model";
}

// 读 16-bit PCM WAV → float [-1,1] 单声道。用于 ZipVoice 的参考音频。
// 多声道取第一声道（不做混音：混音会引入相位差，克隆音色会变）。
bool read_wav_mono_f32(const std::string& path, std::vector<float>& out,
                       int& sample_rate) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string buf((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) != 0) return false;

    auto rd16 = [&](size_t o) {
        return static_cast<int>(static_cast<unsigned char>(buf[o]) |
                                (static_cast<unsigned char>(buf[o + 1]) << 8));
    };
    auto rd32 = [&](size_t o) {
        return static_cast<int>(
            static_cast<unsigned char>(buf[o]) |
            (static_cast<unsigned char>(buf[o + 1]) << 8) |
            (static_cast<unsigned char>(buf[o + 2]) << 16) |
            (static_cast<unsigned>(static_cast<unsigned char>(buf[o + 3])) << 24));
    };

    int channels = 1, bits = 16;
    const char* data = nullptr;
    size_t data_size = 0;
    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const int csize = rd32(pos + 4);
        const size_t body = pos + 8;
        if (csize < 0 || body + static_cast<size_t>(csize) > buf.size()) break;
        if (std::memcmp(buf.data() + pos, "fmt ", 4) == 0 && csize >= 16) {
            channels = rd16(body + 2);
            sample_rate = rd32(body + 4);
            bits = rd16(body + 14);
        } else if (std::memcmp(buf.data() + pos, "data", 4) == 0) {
            data = buf.data() + body;
            data_size = static_cast<size_t>(csize);
        }
        pos = body + static_cast<size_t>(csize) + (csize & 1);  // 奇数长度有 pad
    }
    if (!data || bits != 16 || channels < 1) return false;

    const size_t n = data_size / 2;
    out.resize(n / static_cast<size_t>(channels));
    for (size_t i = 0; i < out.size(); ++i) {
        const unsigned char* s =
            reinterpret_cast<const unsigned char*>(data) + i * 2 * channels;
        out[i] = static_cast<float>(
                     static_cast<int16_t>(static_cast<uint16_t>(s[0] | (s[1] << 8)))) /
                 32768.0f;
    }
    return !out.empty();
}

}  // namespace
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
        c.model.num_threads = 2;
        c.model.debug = 0;
        c.model.provider = p.c_str();
        c.max_num_sentences = 2;

        if (config.engine == "piper") {
            // Piper(VITS)：<onnx> + tokens.txt + espeak-ng-data
            c.model.vits.model = config.model_path.c_str();
            c.model.vits.tokens = config.tokens_path.c_str();
            c.model.vits.data_dir = config.data_dir.c_str();
            c.model.vits.length_scale = std::max(0.25f, config.speed);
            c.model.vits.noise_scale = 0.667f;
            c.model.vits.noise_scale_w = 0.8f;
        } else if (config.engine == "zipvoice") {
            // ZipVoice：encoder + decoder + 独立 vocoder（Vocos）+ tokens/lexicon/espeak
            // 全部 INT8，纯 CPU 即可实时（实测 RTF 0.26~0.61 @ 2 线程）。
            // 音色不靠 sid，靠参考音频 —— 见 Impl 里的 ref_samples_。
            c.model.zipvoice.encoder = config.zipvoice_encoder.c_str();
            c.model.zipvoice.decoder = config.zipvoice_decoder.c_str();
            c.model.zipvoice.vocoder = config.zipvoice_vocoder.c_str();
            c.model.zipvoice.tokens = config.tokens_path.c_str();
            if (!config.lexicon.empty())
                c.model.zipvoice.lexicon = config.lexicon.c_str();
            if (!config.data_dir.empty())
                c.model.zipvoice.data_dir = config.data_dir.c_str();
            c.model.zipvoice.feat_scale = config.feat_scale;
            c.model.zipvoice.t_shift = config.t_shift;
            c.model.zipvoice.target_rms = config.target_rms;
            c.model.zipvoice.guidance_scale = config.guidance_scale;
        } else {
            // Kokoro
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
        }

        tts_ = SherpaOnnxCreateOfflineTts(&c);
        if (!tts_) {
            LOG_ERROR("{}TTS: failed to create engine ({})", config.engine, config.model_path);
            return false;
        }
        provider_ = p;
        sample_rate_ = SherpaOnnxOfflineTtsSampleRate(tts_);
        LOG_INFO("{}TTS: engine ready, sample_rate={}, speakers={}, provider={}",
                 engine_display_name(config.engine), sample_rate_,
                 SherpaOnnxOfflineTtsNumSpeakers(tts_), prov);
        return true;
    }
#endif

    // ---- ZipVoice 参考音频（零样本克隆的"音色"来源）----
    // 存在这里而不是每次合成时读盘：参考音频是**固定的一段声音**，
    // 每段合成都读一次文件 + 解析 WAV 是纯浪费（几十 KB 起步）。
    std::vector<float> ref_samples_;
    int ref_sample_rate_{0};

    // 读参考音频并**就地更新**缓存。单独抽出来是因为 set_reference_voice()
    // 要在"验证成功后才改配置"的前提下先读一遍——顺序反了会留下
    // 新配置配旧音色的不一致状态。
    bool read_reference_wav(const std::string& path, std::vector<float>& out,
                            int& sr) {
        if (!read_wav_mono_f32(path, out, sr)) return false;
        ref_samples_ = std::move(out);
        ref_sample_rate_ = sr;
        cfg_.ref_audio_path = path;
        return true;
    }

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
        if (config.engine == "piper") {
            // Piper(VITS) 固定 CPU：Intel Arc DirectML 对 grouped ConvTranspose 有已知崩溃，
            // 与 Kokoro 相同规避策略；且 Piper 体积小，CPU 已可实时合成。
            LOG_INFO("PiperTTS: 固定 CPU 推理（DirectML 兼容性考虑）");
            prov = "cpu";
        } else if (config.engine == "zipvoice") {
            // ZipVoice 固定 CPU。理由与 Piper 同类但更充分：
            //   ① 全 INT8 + 123M，CPU 上实测 RTF 0.26~0.61（已比实时快）；
            //   ② Intel Arc 的 DirectML 在本项目上已两次踩坑（Kokoro 的
            //      grouped ConvTranspose 直接 0xC0000409），没必要再冒险；
            //   ③ Flow Matching 的计算图形状随文本长度变化，DML 上大概率
            //      还要再编译，收益不明而崩溃风险确定。
            LOG_INFO("ZipVoiceTTS: 固定 CPU 推理（INT8，零样本克隆）");
            prov = "cpu";
        } else {
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
        }

        // ZipVoice 的音色来自参考音频，这里一次性载入。
        // 失败必须早报：没有参考音频的零样本克隆是**沉默**，不是降级。
        if (config.engine == "zipvoice") {
            std::vector<float> tmp;
            int sr = 0;
            if (!read_reference_wav(config.ref_audio_path, tmp, sr)) {
                LOG_ERROR("ZipVoiceTTS: 无法读取参考音频 '{}'（克隆音色需要它）",
                          config.ref_audio_path);
                return false;
            }
            if (config.ref_text.empty()) {
                LOG_ERROR("ZipVoiceTTS: 缺少参考音频转写文本（ref_text）—— "
                          "sherpa-onnx 要求音频与文本严格对应，不匹配会明显降质");
                return false;
            }
            LOG_INFO("ZipVoiceTTS: 参考音频 {:.2f}s @ {} Hz, ref_text_len={}",
                     ref_samples_.size() / static_cast<double>(ref_sample_rate_),
                     ref_sample_rate_, config.ref_text.size());
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

            if (config.engine == "zipvoice") {
                // 零样本克隆：音色来自参考音频 + 其转写文本，没有 sid 概念。
                g.reference_audio = ref_samples_.data();
                g.reference_audio_len = static_cast<int32_t>(ref_samples_.size());
                g.reference_sample_rate = ref_sample_rate_;
                g.reference_text = config.ref_text.c_str();
                // num_steps=4 是官方推荐值。给2 更快但音质明显下降；
                // 给 6+ 只会线性变慢（实测 RTF 0.59→0.98），不值。
                g.num_steps = config.num_steps > 0 ? config.num_steps : 4;
                // 刻意不传 extra：min_char_in_sentence 用官方默认 30。
                // 它是"把短句合并到此长度"的下限，调小只会让切分碎片化
                // （实测设成 2 时日志出现 OOV 与 token 转换失败）。
            }
            return SherpaOnnxOfflineTtsGenerateWithConfig(tts_, text.c_str(), &g,
                                                          nullptr, nullptr);
        };

        const SherpaOnnxGeneratedAudio* audio = generate();
        // Intel Arc + DirectML 对 Kokoro 的 grouped ConvTranspose 不支持（0x80070057），
        // 推理时才会失败；DML 失败则回退 CPU 重建引擎并重试一次。
        if ((!audio || audio->n <= 0) && provider_ == "directml") {
            if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
            LOG_WARN("{}TTS: DirectML 推理失败，回退 CPU 重建引擎重试",
                     engine_display_name(config.engine));
            if (!create_engine(cfg_, "cpu")) return {};
            audio = generate();
        }
        if (!audio || audio->n <= 0) {
            LOG_WARN("{}TTS: generation failed for '{}'",
                     engine_display_name(config.engine), text);
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
        LOG_INFO("{}TTS: '{}' -> {} samples @ {} Hz ({} chars, provider={})",
                 engine_display_name(config.engine), text, out.size(),
                 audio->sample_rate, text.size(), provider_);
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

void TTS::feed_chunks_(const std::vector<int16_t>& audio,
                       const TTSCallback& callback) {
    if (!callback) return;
    if (audio.empty()) {
        callback(nullptr, 0, true);
        return;
    }
    // 200ms @ 24kHz。bridge 侧一次可能返回好几秒的音频，
    // 一次性喂给声卡会让播放队列排很久、barge-in 变得迟钝。
    constexpr size_t kChunk = 4800;
    for (size_t i = 0; i < audio.size(); i += kChunk) {
        const size_t end = std::min(i + kChunk, audio.size());
        callback(audio.data() + i, end - i, end >= audio.size());
        // 50ms 是喂给播放缓冲的节奏，不是"睡 50ms 浪费时间"——
        // AudioRouter 内部有队列，这里只是别把整个音频一口气灌爆它。
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool TTS::initialize(const TTSConfig& config) {
    config_ = config;
    requested_engine_ = config.engine;
    bridge_.reset();
    bridge_engine_ = TtsBridgeEngine::Unknown;

    // ---- §24：PyTorch 引擎（qwen3tts / chatterbox）走本地 Python bridge ----
    // 这两个引擎没有原生 C++ 推理后端。这里做一次健康探测：
    //   通过 → 建bridge，synthesize 时把 adapter 算出的 instruction /
    //          exaggeration / cfg_weight 真正喂给模型（不再只是"算好放着"）。
    //   不通过 → **就地降级**成 simple（SAPI）继续发声，而不是让整个 TTS 失败。
    //          降级只改引擎类型，config_.prosody_adapter 保留，
    //          这样上层仍按原引擎理解韵律，只是实际发声回退到系统语音。
    // 就地降级（而不是双轨并存）是有意的：两个引擎同时活着会让
    // "现在到底是谁在发声"变得无法回答。
    TtsBridgeEngine want = tts_bridge_engine_from_string(config.engine);
    if (want != TtsBridgeEngine::Unknown) {
        TtsBridgeConfig bc;
        bc.endpoint = config.bridge_endpoint;
        bc.engine = want;
        bc.timeout_ms = config.bridge_timeout_ms;
        bc.health_timeout_ms = config.bridge_health_timeout_ms;
        bc.sample_rate = config.sample_rate;

        auto probe_bridge = std::make_unique<TtsBridge>(bc);
        std::string detail;
        // 给 3 秒预算等权重加载：Python bridge 端口秒开、后台加载权重，
        // 首次启动可能要几十秒。启动时多等几秒好过整个会话都降级。
        const TtsBridgeHealth h = probe_bridge->probe(&detail, 3000);
        if (h == TtsBridgeHealth::Ready) {
            bridge_ = std::move(probe_bridge);
            bridge_engine_ = want;
            LOG_INFO("TTS: engine={} via Python bridge at {}",
                     tts_bridge_engine_name(want), bc.endpoint);
        } else if (h == TtsBridgeHealth::Loading) {
            // 还在加载：仍然接上 bridge，让首句去承担这个等待
            //（而不是启动时白等一轮）。可用性按"未就绪"记，
            // 这样 provider_label 不会把没准备好的引擎说成已生效。
            bridge_ = std::move(probe_bridge);
            bridge_engine_ = want;
            LOG_INFO("TTS: engine={} bridge at {} still loading ({}), "
                     "first synth will wait", tts_bridge_engine_name(want),
                     bc.endpoint, detail);
        } else {
            LOG_WARN("TTS: engine={} bridge unavailable ({}), "
                     "falling back to system voice (SAPI)",
                     config.engine, detail);
            config_.engine = "simple";
        }
    }

    simple_engine_ = (config_.engine == "simple");
    impl_ = std::make_unique<Impl>();

    if (!impl_->load(config_)) {
        LOG_ERROR("TTS: failed to load model");
        return false;
    }

    LOG_INFO("TTS: engine={} init, speed={}, lang={}, speaker={}",
             config_.engine, config_.speed, config_.lang, config_.speaker_id);
    return true;
}

std::vector<int16_t> TTS::synthesize(const std::string& text) {
    if (!impl_) return {};

    // bridge 路径：整段合成，失败则回退到底层引擎。
    // 回退是"这一段没声音"而不是"整个引擎报废" —— 一句合成都可能因
    // 文本含特殊符号而失败，丢掉整轮体验太差。
    if (bridge_ && !text.empty()) {
        std::vector<int16_t> out;
        std::string err;
        if (bridge_->synthesize(text, bridge_controls_, out, &err))
            return out;
        if (!err.empty())
            LOG_WARN("TTS: bridge synth failed ({}), falling back", err);
    }

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

    // bridge 引擎（qwen3tts / chatterbox）：走 Python 推理。失败则整段
    // 回退到底层声学引擎 —— 注意回退时必须回调一次 is_last，
    // 否则 ResponsePlan 的播放游标永远停在 Pending，状态机会卡住。
    if (bridge_ && !text.empty()) {
        std::vector<int16_t> audio;
        std::string err;
        if (bridge_->synthesize(text, bridge_controls_, audio, &err)) {
            feed_chunks_(audio, callback);
            last_synthesize_ms.store(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            synthesizing_ = false;
            return;
        }
        if (err.empty()) {
            // 被 barge-in 打断：不是错误，也不该有声音。
            LOG_DEBUG("TTS: bridge interrupted, dropping segment");
        } else {
            LOG_WARN("TTS: bridge synth failed ({}), falling back", err);
        }
        if (callback) callback(nullptr, 0, true);
        last_synthesize_ms.store(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0)
                .count());
        synthesizing_ = false;
        return;
    }

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

    feed_chunks_(impl_->synthesize(config_, text), callback);

    last_synthesize_ms.store(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count());
    synthesizing_ = false;
}

void TTS::stop() {
    synthesizing_ = false;
    // 打断 bridge 侧正在进行的推理：只设标志位，Python 端在下一段检查。
    // 不等它真的返回 —— 打断路径上的任何阻塞都会直接变成用户感知到的卡顿。
    if (bridge_) bridge_->interrupt();
    if (impl_) {
        impl_->stop();
    }
}

void TTS::set_cancel_token(std::shared_ptr<CancelToken> token) {
    cancel_token_ = std::move(token);
    if (bridge_) bridge_->set_cancel_token(token);
}

void TTS::set_speed(float speed) {
    if (speed < 0.25f) speed = 0.25f;
    if (speed > 2.0f) speed = 2.0f;
    config_.speed = speed;                       // Kokoro 后续合成立即按新语速
    bridge_controls_.speed = speed;              // bridge 侧同样按新语速
    if (impl_ && impl_->sapi) impl_->sapi->set_rate(speed);   // SAPI 同步映射
}

void TTS::set_bridge_controls(const TtsBridgeControls& controls) {
    bridge_controls_ = controls;
    // speed 在两处都有意义：SAPI/模型引擎走config_.speed，bridge 走controls。
    if (controls.speed > 0.0f) config_.speed = controls.speed;
}

const TtsBridgeControls& TTS::bridge_controls() const {
    return bridge_controls_;
}

bool TTS::bridge_available() const {
    return bridge_ && bridge_->available();
}

std::string TTS::bridge_engine_name() const {
    return tts_bridge_engine_name(bridge_engine_);
}

std::string TTS::bridge_endpoint() const {
    return bridge_ ? bridge_->config().endpoint : std::string{};
}

void TTS::set_speaker_id(int id) {
    if (id < 0) id = 0;
    config_.speaker_id = id;                     // Kokoro 后续合成立即切换发音人
}

bool TTS::set_reference_voice(const std::string& wav_path,
                              const std::string& ref_text) {
    if (wav_path.empty() || ref_text.empty()) {
        LOG_WARN("TTS: set_reference_voice 需要 wav 路径与转写文本");
        return false;
    }
    // 先验证能读出来再改配置：半途失败会留下"新配置 + 旧音色"的不一致状态。
    std::vector<float> samples;
    int sr = 0;
    if (!impl_ || !impl_->read_reference_wav(wav_path, samples, sr)) {
        LOG_ERROR("TTS: 无法读取参考音频 '{}'", wav_path);
        return false;
    }
    config_.ref_audio_path = wav_path;
    config_.ref_text = ref_text;
    LOG_INFO("TTS: 克隆音色已切换 ({:.2f}s @ {} Hz, ref_text_len={})",
             samples.size() / static_cast<double>(sr), sr, ref_text.size());
    return true;
}

void TTS::set_num_steps(int steps) {
    if (steps < 1) steps = 1;
    if (steps > 16) steps = 16;      // 更大只会线性变慢，没有收益
    config_.num_steps = steps;
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
    // bridge 优先级最高：引擎虽然名义上是 qwen3tts/chatterbox，
    // 但真正发声的可能是降级后的 SAPI，所以要先看 bridge 是否真的活着。
    if (bridge_) {
        std::string label = std::string("Python bridge (") +
                            tts_bridge_engine_name(bridge_engine_) + ")";
        if (!bridge_->available())
            label += " [未就绪]";
        return label;
    }
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

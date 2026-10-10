// src/tts/tts.hpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "core/types.hpp"
#include "core/cancel_token.hpp"
#include "tts/tts_bridge.hpp"

namespace voice_agent {

class SapiSpeaker;

// TTS 配置
struct TTSConfig {
    std::string engine = "simple";  // "simple"=Windows 系统语音(SAPI，免模型/流式默认)；
                                    // "kokoro"=本地 Kokoro 模型；"piper"=本地 Piper(VITS) 模型；
                                    // "zipvoice"=ZipVoice 零样本克隆（INT8，纯 CPU 可跑）；
                                    // "qwen3tts"/"chatterbox"=PyTorch 引擎，走本地 Python bridge
    std::string model_path;      // 模型路径（Kokoro: model.onnx；Piper: <voice>.onnx）
    std::string voice_path;       // 音色路径（Kokoro: voices.bin）
    std::string tokens_path;      // token 表（Kokoro/Piper: tokens.txt）
    std::string data_dir;         // espeak-ng-data 目录
    std::string lexicon;          // 可选词典（Kokoro: lexicon-zh.txt）
    int speaker_id = 45;          // 发音人 ID（Kokoro 多语言：45=zf_xiaobei 中文女声）
    float speed = 1.0f;          // 语速
    float pitch = 1.0f;          // 音调
    float volume = 1.0f;         // 音量
    int sample_rate = 24000;      // 输出采样率
    std::string lang = "zh";      // 语言
    std::string provider = "auto"; // auto: DirectML 可用则 GPU，否则 CPU；可选 dml / cpu

    // ---- ZipVoice（零样本音色克隆，纯 CPU INT8）----
    // 与其它引擎最大的不同：没有"发音人 ID"，音色来自**参考音频 + 其转写文本**。
    // 两者必须严格对应，不匹配时音质会明显下降（sherpa-onnx 官方明确警告）。
    std::string zipvoice_encoder;   // encoder.int8.onnx
    std::string zipvoice_decoder;   // decoder.int8.onnx
    std::string zipvoice_vocoder;   // vocos_24khz.onnx（独立声码器，所有音色共用）
    std::string ref_audio_path;     // 参考音频 wav
    std::string ref_text;           // 参考音频的**逐字**转写
    int num_steps = 4;              // Flow Matching 步数。4=官方推荐；2 更快但音质降
    float guidance_scale = 1.5f;    // CFG 引导强度
    float t_shift = 0.4f;           // 时长偏移（越大越慢）
    float feat_scale = 1.0f;        // 特征缩放
    float target_rms = 0.03f;       // 目标 RMS（响度对齐参考音频）

    // 韵律适配器选择（§24）：决定语音"怎么说"的那一层。
    // "auto" = 按引擎类型自动（SAPI→SapiTtsAdapter，模型引擎→ModelTtsAdapter）；
    // "qwen3tts" / "chatterbox" = 使用对应风格适配器。
    std::string prosody_adapter = "auto";

    // ---- qwen3tts / chatterbox（PyTorch bridge）----
    // 这两个引擎没有原生 C++ 后端，推理交给独立 Python 进程
    // （scripts/tts_bridge_server.py）。bridge 不可用时**自动回退**到
    // simple/kokoro/piper，不影响可用性。
    std::string bridge_endpoint = "http://127.0.0.1:8770";
    int bridge_timeout_ms = 30000;      // 首次合成要加载权重，给足时间
    int bridge_health_timeout_ms = 1500; // 健康探测必须快，否则卡住启动
};

// TTS 音频回调
using TTSCallback = std::function<void(const int16_t* audio, size_t frames, bool is_last)>;

class TTS {
public:
    TTS();
    ~TTS();

    // 初始化 TTS
    bool initialize(const TTSConfig& config);

    // 合成语音（阻塞）
    std::vector<int16_t> synthesize(const std::string& text);

    // 流式合成
    void synthesize_stream(const std::string& text, TTSCallback callback);

    // 中断合成
    void stop();

    // 设置取消令牌
    void set_cancel_token(std::shared_ptr<CancelToken> token);

    // 是否正在合成
    bool is_synthesizing() const { return synthesizing_; }

    // simple(SAPI) 引擎是否正在实际出声（含队列中待读的段）。
    // SAPI 的朗读不经过 AudioRouter，状态机需要据此判断"语音还没读完"，
    // 否则会提前收尾（表现为只读第一句）。其余引擎恒为 false ——
    // 它们的音频走 AudioRouter，播放回调自己知道缓冲何时放完。
    bool is_speaking() const;

    // 是否使用"简单引擎"（Windows 系统语音 SAPI，免模型、实时流式）
    bool simple_engine() const { return simple_engine_; }

    // 当前引擎名："simple" / "kokoro" / "piper" / "qwen3tts" / "chatterbox"
    // （qwen3tts/chatterbox 在 bridge 探测失败时会被就地降级成 "simple"）
    std::string engine_name() const { return config_.engine; }

    // **用户请求**的引擎名，与实际生效的引擎名区分开。
    // bridge 不可用时 engine_name() 返回降级后的 "simple"，但韵律语义
    // 仍应按 requested 的引擎理解（instruction 照算，只是由 SAPI 发声）。
    std::string requested_engine() const { return requested_engine_; }

    // 简单引擎模式下，朗读结束回调（SAPI 播完一段后触发一次，用于驱动状态机）
    void set_speech_done_callback(std::function<void()> cb);

    // 当前朗读中的系统语音名称（仅简单引擎）
    std::string simple_voice_name() const;

    // 是否运行在真实模型（非 Mock）后端
    bool uses_real_backend() const;

    // 生效推理后端标签："DirectML GPU" / "CPU" / "系统语音(SAPI)" / "Mock"
    std::string provider_label() const;

    // 获取配置
    const TTSConfig& config() const { return config_; }

    // 动态调整语速（倍率，0.25~2.0）。简单引擎(SAPI)同步映射语速并即时生效；
    // Kokoro 对后续合成立即生效（无需重建引擎）。
    void set_speed(float speed);

    // ---- bridge 引擎控制通道（§24）----
    // 适配器（Qwen3TtsAdapter / ChatterboxAdapter）在 apply_prosody 里算出
    // 引擎特有控制后调这个方法存起来，synthesize 时由 bridge 消费。
    // 为什么走TTS 而不是让 adapter 直接调bridge：bridge 的生命周期
    // （探测/降级/打断）归 TTS 管，adapter 不该知道它的存在。
    void set_bridge_controls(const TtsBridgeControls& controls);
    const TtsBridgeControls& bridge_controls() const;

    // 动态切换发音人 ID（Kokoro 生效；简单引擎忽略）。
    void set_speaker_id(int id);

    // 运行时切换 ZipVoice 的克隆音色（换参考音频 + 其转写文本）。
    // 不用重建引擎 —— 参考音频只是生成时的输入，模型权重不变。
    // 两个参数必须来自同一段音频，否则音质会明显下降。
    bool set_reference_voice(const std::string& wav_path,
                             const std::string& ref_text);

    // ZipVoice 流式步数。2=更快（音质降）/ 4=官方推荐 / 6+=线性变慢不值。
    void set_num_steps(int steps);

    // 该引擎是否走Python bridge（qwen3tts / chatterbox）。
    bool uses_bridge() const { return bridge_ != nullptr; }

    // bridge 探测是否通过。未探测过 = 尚未 init。
    bool bridge_available() const;

    // 供上层/日志查询bridge 的引擎名与端点
    std::string bridge_engine_name() const;
    std::string bridge_endpoint() const;

    // 最近一次合成耗时（毫秒，供调试面板轮询）
    std::atomic<double> last_synthesize_ms{0.0};

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TTSConfig config_;
    // initialize() 时的请求引擎名（见 requested_engine()）
    std::string requested_engine_;
    std::atomic<bool> synthesizing_{false};
    std::atomic<bool> simple_engine_{false};
    std::shared_ptr<CancelToken> cancel_token_;

    // bridge（qwen3tts / chatterbox 专用）。为 nullptr 表示当前引擎不走 bridge。
    std::unique_ptr<class TtsBridge> bridge_;
    // bridge 引擎；Unknown 表示不走 bridge
    TtsBridgeEngine bridge_engine_{TtsBridgeEngine::Unknown};
    // 最近一次由适配器写入的引擎控制（instruction / exaggeration / ...）
    TtsBridgeControls bridge_controls_;

    // 把整段音频切成播放回调。bridge 与本地模型两条路共用，
    // 保证"chunk 大小 + is_last 语义"只存在一份实现。
    static void feed_chunks_(const std::vector<int16_t>& audio,
                             const TTSCallback& callback);
};

// 创建 TTS 实例
std::unique_ptr<TTS> create_kokoro_tts();

}  // namespace voice_agent

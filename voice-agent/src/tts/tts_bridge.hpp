// src/tts/tts_bridge.hpp
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/cancel_token.hpp"

namespace voice_agent {

// ========== TTS 推理 bridge 客户端（§24 补完）============
//
// Qwen3-TTS / Chatterbox 都是 PyTorch 模型，没有原生 C++ 推理后端。
// 本类负责与本地 Python bridge 进程（scripts/tts_bridge_server.py）通信：
// POST /synthesize  {text, instruction, exaggeration, cfg_weight, speed, ...}
//     → 200 audio/wav（单声道 int16 PCM）
//
// 设计上的三个要点：
//   1. **进程隔离**：Python 崩了/OOM 了只是这一轮没声音，C++ 主进程还在。
//      这是选"独立进程 + HTTP"而不是"嵌进主进程"的核心原因。
//   2. **降级不隐瞒**：bridge 不可用时 available() 返回 false，由 TTS 回退到
//      底层声学引擎；所有失败原因都带回来写日志，不静默。
//   3. **纯函数可单测**：WAV 解析与 JSON 组装都拆成自由函数，
//      不需要真的起服务就能精确断言（见tests/test_tts_bridge.cpp）。

// bridge 支持的引擎
enum class TtsBridgeEngine {
    Unknown = 0,
    Qwen3Tts,      // 自然语言 instruction 驱动风格
    Chatterbox,    // exaggeration / cfg_weight 数值旋钮
};

TtsBridgeEngine tts_bridge_engine_from_string(const std::string& s);
const char* tts_bridge_engine_name(TtsBridgeEngine e);

// ========== 发送给 bridge 的控制量============
// 与 Qwen3Controls / ChatterboxControls 分开而不是合并成一个带全部字段的
// 结构体：合并后"这个引擎到底会不会用 exaggeration"就没人说得清了。
// 这里用「都带上、bridge 侧按 engine 决定消费哪些」的方式，
// 与 Python 端_synth_qwen3 / _synth_chatterbox 的分工一致。
struct TtsBridgeControls {
    std::string instruction;    // Qwen3-TTS：自然语言风格指令
    std::string voice;          // Qwen3-TTS：发音人
    float speed{1.0f};          // 通用语速
    float exaggeration{0.5f};   // Chatterbox：情绪强度0~1
    float cfg_weight{0.5f};     // Chatterbox：克制/清晰 0.1~1
    std::string lang{"zh"};
};

// bridge 配置
struct TtsBridgeConfig {
    std::string endpoint{"http://127.0.0.1:8770"};
    TtsBridgeEngine engine{TtsBridgeEngine::Unknown};
    int timeout_ms{30000};      // 首次合成要加载权重，给足时间
    int health_timeout_ms{1500}; // 健康探测必须快，否则卡住启动流程
    int sample_rate{24000};     // 与 orchestrator.playback_sample_rate 一致
};

// ---- 纯函数（可离线单测）----

// 解析 WAV（RIFF/WAVE）→ 单声道 int16 PCM。
// 只支持 16-bit PCM（bridge 侧固定输出 PCM_16）；其他格式返回 false。
// 支持多声道（取第一声道）与WAVE_FORMAT_EXTENSIBLE。
bool parse_wav_pcm16(const std::string& wav, std::vector<int16_t>& out,
                     int& sample_rate, int& channels);

// 线性重采样（voice-agent 播放链路固定 24kHz，bridge 输出可能不是）。
std::vector<int16_t> resample_linear(const std::vector<int16_t>& in,
                                     int src_rate, int dst_rate);

// 组装 /synthesize 的 JSON 请求体。
std::string build_synthesize_body(const std::string& text,
                                  const TtsBridgeControls& controls,
                                  int sample_rate);

// bridge 探测结果。三态而不是 bool —— "正在加载几十秒"和"彻底不可用"
// 必须区别对待：前者等一下就能用，后者只能回退。二值判断会把前者
// 误判成后者，导致整个会话永久降级（启动竞态，见 TtsBridge::probe）。
enum class TtsBridgeHealth {
    Ready,      // 后端就绪，可以合成
    Loading,    // 正在加载权重，稍后重试
    Unavailable // 不可用（没起bridge / 缺包 / 没权重），应回退
};

const char* tts_bridge_health_name(TtsBridgeHealth h);

// 从 /health 的 JSON 解析探测结果。解析失败视为 Unavailable（保守：
// 宁可回退发声，也不要拿到一个说不清状态的引擎）。
TtsBridgeHealth parse_health_state(const std::string& json_body,
                                   std::string* detail);

// ========== 客户端 ==========
class TtsBridge {
public:
    explicit TtsBridge(TtsBridgeConfig cfg);
    ~TtsBridge();

    // 探测 bridge 状态。注意：**不**触发模型加载 —— Python 侧是懒加载，
    // health 只回答"进程在不在 / 后端到哪一步了"。
    // 状态为 Loading 时会在 wait_budget_ms 内轮询等待（默认 0 = 不等）。
    TtsBridgeHealth probe(std::string* detail = nullptr,
                          int wait_budget_ms = 0);

    // 上次探测的结论
    TtsBridgeHealth health() const { return health_; }
    bool available() const { return health_ == TtsBridgeHealth::Ready; }

    // 合成一段。成功返回 true 且 out 非空。
    // 失败时err 里是可直接读的原因（已写日志）。
    bool synthesize(const std::string& text, const TtsBridgeControls& controls,
                    std::vector<int16_t>& out, std::string* err = nullptr);

    // 打断 bridge侧正在进行的合成（barge-in 用）。fire-and-forget。
    void interrupt();

    // 绑定取消令牌：打断时正在飞行的 HTTP 请求会被取消，
    // 不必等到 bridge 那边合成完才发现"没人要这段了"。
    void set_cancel_token(std::shared_ptr<CancelToken> token) {
        cancel_ = std::move(token);
    }

    const TtsBridgeConfig& config() const { return cfg_; }

private:
    TtsBridgeConfig cfg_;
    TtsBridgeHealth health_{TtsBridgeHealth::Unavailable};
    std::shared_ptr<CancelToken> cancel_;
};

}  // namespace voice_agent
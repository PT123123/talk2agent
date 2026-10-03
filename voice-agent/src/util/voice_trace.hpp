// src/util/voice_trace.hpp
#pragma once
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace voice_agent {

using json = nlohmann::json;

// ========== 时序事件 ==========
// 一次对话的完整时间线。用于回答"这次为什么慢/为什么没打断"这类问题。
struct TraceEvent {
    uint64_t t_us{0};          // 相对本轮开始的微秒
    std::string type;          // USER_SPEECH_START / ASR_PARTIAL / TURN_COMPLETE ...
    std::string detail;
    double ms{0.0};            // 该步骤耗时（可选）

    json to_json() const;
};

// ========== 一轮对话的完整记录 ==========
struct TurnRecord {
    std::string session_id;
    std::string turn_id;
    std::string user_text;
    std::string user_intent;      // UserSpeechIntent 名字
    std::string policy_action;    // ResponseAction 名字
    std::string policy_reason;
    std::string route_tier;       // ModelTier 名字
    std::string route_engine;
    bool        spoke_ack{false}; // 是否抢答过
    std::string ack_text;

    // 延迟指标（毫秒；-1 = 未采集到）
    double speech_to_asr_ms{-1.0};     // 语音结束 → 转写完成
    double asr_to_first_audio_ms{-1.0};// 转写完成 → 首段音频
    double llm_ttft_ms{-1.0};         // 生成开始 → 首 token
    double tts_first_audio_ms{-1.0};  // 合成开始 → 首段音频
    double total_ms{-1.0};            // 轮次总耗时

    bool interrupted{false};
    int  discard_count{0};            // 响应修订丢弃的段数
    std::vector<TraceEvent> events;

    json to_json() const;
};

// ========== VoiceTraceWriter ==========
// 写 JSONL（一行一轮次）。目的：离线回放与跨实现对比。
//
// 设计要点：
//   1. **流式落盘**：每轮结束立即写一行并 flush。崩溃时已完成的轮次不丢
//      —— 长跑测试崩了以后还能看到崩之前发生了什么。
//   2. **可关闭**：不要 trace 时零开销（enabled_ 短路）。
//   3. **线程安全**：on_llm_token_ 在多个线程被调用。
class VoiceTraceWriter {
public:
    explicit VoiceTraceWriter(std::string path, bool enabled = true);
    ~VoiceTraceWriter();

    VoiceTraceWriter(const VoiceTraceWriter&) = delete;
    VoiceTraceWriter& operator=(const VoiceTraceWriter&) = delete;

    bool enabled() const { return enabled_; }
    bool ok() const { return static_cast<bool>(file_); }

    // 写一轮（线程安全）
    void write_turn(const TurnRecord& rec);

    // 写一条独立事件（如启动、停止、错误）
    void write_event(const TraceEvent& ev, const std::string& session_id = {});

    // 会话头（启动时写一次）
    void write_session_header(const json& info);

private:
    std::string path_;
    bool enabled_{false};
    std::mutex mutex_;
    std::ofstream file_;
    std::atomic<uint64_t> turn_counter_{0};
};

// ========== VoiceTraceReader ==========
// 读回 JSONL，用于离线分析。
class VoiceTraceReader {
public:
    explicit VoiceTraceReader(const std::string& path);

    bool ok() const { return file_.good(); }

    // 读全部轮次（跳过非轮次行）
    std::vector<json> turns() const;

    // 统计：延迟分位数（p50/p90/p99）
    struct LatencyStats {
        size_t count{0};
        double p50{0}, p90{0}, p99{0}, min{0}, max{0}, mean{0};
    };
    static LatencyStats compute_latency(const std::vector<json>& turns,
                                        const std::string& field);

    // 汇总：错误轮次数、被打断次数、抢答次数
    struct Summary {
        size_t turns{0};
        size_t interrupted{0};
        size_t acked{0};
        size_t failed{0};
        size_t superseded{0};
    };
    static Summary summarize(const std::vector<json>& turns);

private:
    std::string path_;
    mutable std::ifstream file_;
};

}  // namespace voice_agent

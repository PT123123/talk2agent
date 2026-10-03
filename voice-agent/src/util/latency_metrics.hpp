// src/util/latency_metrics.hpp
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace voice_agent {

// ========== 指标定义 ==========
// 这些是"自然度到底靠不靠得住"的量化依据。不要靠"主观感觉不错"验收。
enum class MetricKind {
    SpeechToAsr,        // 语音结束 → 转写完成
    AsrToFirstAudio,    // 转写完成 → 首段音频（**最影响体感**）
    LlmTtft,            // 生成开始 → 首 token
    TtsFirstAudio,      // 合成开始 → 首段音频
    TurnTotal,          // 轮次总耗时
    BargeInLatency,     // 用户开口 → 停止播放
    EouDelay,           // 语音结束 → 判定为轮次结束
    ToolLatency,        // 单个工具耗时
    SearchLatency,      // 搜索耗时
    MemoryLatency,      // 记忆召回耗时
    TtsSynthesize       // 单段合成耗时
};

inline const char* metric_kind_name(MetricKind k) {
    switch (k) {
        case MetricKind::SpeechToAsr:      return "speech_to_asr";
        case MetricKind::AsrToFirstAudio:  return "asr_to_first_audio";
        case MetricKind::LlmTtft:          return "llm_ttft";
        case MetricKind::TtsFirstAudio:    return "tts_first_audio";
        case MetricKind::TurnTotal:        return "turn_total";
        case MetricKind::BargeInLatency:   return "barge_in_latency";
        case MetricKind::EouDelay:         return "eou_delay";
        case MetricKind::ToolLatency:      return "tool_latency";
        case MetricKind::SearchLatency:    return "search_latency";
        case MetricKind::MemoryLatency:    return "memory_latency";
        case MetricKind::TtsSynthesize:    return "tts_synthesize";
    }
    return "unknown";
}

inline constexpr int kMetricKindCount = 10;

// ========== 指标快照 ==========
struct MetricSnapshot {
    int64_t count[kMetricKindCount]{};
    double  sum[kMetricKindCount]{};
    double  min_ms[kMetricKindCount]{};
    double  max_ms[kMetricKindCount]{};

    // 达标率（如"barge-in < 250ms"）
    int64_t within_budget[kMetricKindCount]{};
    int64_t budget_ms[kMetricKindCount]{};

    double avg(int i) const { return count[i] ? sum[i] / double(count[i]) : 0.0; }
};

// ========== LatencyMetrics ==========
// 线程安全的指标聚合器。定长环形缓冲，内存有界（长跑必需）。
class LatencyMetrics {
public:
    struct Config {
        // 每个指标的样本容量（环形缓冲）。超过后覆盖最旧的，
        // 用于算分位数；count/sum/min/max 仍为全量累计。
        size_t sample_capacity{512};
        // 达标阈值（毫秒）。<=0 表示该指标不设预算。
        int64_t budget_ms[kMetricKindCount]{};
    };

    explicit LatencyMetrics(Config cfg = {}) : cfg_(cfg) {
        for (int i = 0; i < kMetricKindCount; ++i) {
            if (cfg.budget_ms[i] <= 0) cfg_.budget_ms[i] = default_budget_(i);
        }
    }

    // 记录一次观测
    void record(MetricKind kind, double ms);

    // 分位数（0~1）。p50 / p90 / p99
    double percentile(MetricKind kind, double p) const;

    int64_t count(MetricKind kind) const;
    double avg(MetricKind kind) const;
    double max(MetricKind kind) const;
    double min(MetricKind kind) const;
    int64_t within_budget(MetricKind kind) const;
    int64_t budget_ms(MetricKind kind) const;
    double budget_hit_rate(MetricKind kind) const;

    MetricSnapshot snapshot() const;

    // 人类可读的一行摘要（写进 trace / 日志）
    std::string summary_line() const;

    void reset();

    // 便捷计时器
    // RAII：析构时自动 record。
    // 用法：LatencyTimer t(metrics_, MetricKind::LlmTtft); ... 作用域结束即记录
    class Timer {
    public:
        Timer(LatencyMetrics& m, MetricKind k)
            : metrics_(&m), kind_(k), start_(std::chrono::steady_clock::now()) {}
        ~Timer() {
            if (metrics_ && !stopped_) {
                metrics_->record(kind_,
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - start_)
                                     .count());
            }
        }
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

        double elapsed_ms() const {
            return std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - start_)
                .count();
        }
        // 手动停止（重复停止只生效一次）
        void stop() {
            if (stopped_) return;
            stopped_ = true;
            if (metrics_) metrics_->record(kind_, elapsed_ms());
        }

    private:
        LatencyMetrics* metrics_;
        MetricKind kind_;
        std::chrono::steady_clock::time_point start_;
        bool stopped_{false};
    };

private:
    static int idx(MetricKind k) { return static_cast<int>(k); }
    static int64_t default_budget_(int i);

    Config cfg_;
    mutable std::mutex mutex_;
    std::vector<double> samples_[kMetricKindCount];
    int64_t count_[kMetricKindCount]{};
    double  sum_[kMetricKindCount]{};
    double  min_ms_[kMetricKindCount]{};
    double  max_ms_[kMetricKindCount]{};
    int64_t within_[kMetricKindCount]{};
};

}  // namespace voice_agent

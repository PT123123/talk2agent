// src/util/latency_metrics.cpp
#include "util/latency_metrics.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace voice_agent {

// 默认预算：按"用户能否忍受"设定，不是按技术指标。
// 参考依据：对话系统的可接受延迟通常在 200ms（察觉）~ 1s（明显察觉）之间，
// 超过 2s 用户会以为坏了。
int64_t LatencyMetrics::default_budget_(int i) {
    switch (static_cast<MetricKind>(i)) {
        case MetricKind::SpeechToAsr:      return 1500;   // 转写不该比说完整句话还久
        case MetricKind::AsrToFirstAudio:  return 2000;   // **体感最关键的一条**
        case MetricKind::LlmTtft:          return 1500;
        case MetricKind::TtsFirstAudio:    return 1200;
        case MetricKind::TurnTotal:        return 10000;
        case MetricKind::BargeInLatency:   return 250;    // 实施计划里的硬指标
        case MetricKind::EouDelay:         return 900;    // eou_force_ms
        case MetricKind::ToolLatency:      return 5000;
        case MetricKind::SearchLatency:    return 2500;
        case MetricKind::MemoryLatency:    return 500;
        case MetricKind::TtsSynthesize:    return 3000;
    }
    return 2000;
}

void LatencyMetrics::record(MetricKind kind, double ms) {
    if (ms < 0.0) return;
    const int i = idx(kind);
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_[i] == 0) {
        min_ms_[i] = ms;
        max_ms_[i] = ms;
    } else {
        min_ms_[i] = std::min(min_ms_[i], ms);
        max_ms_[i] = std::max(max_ms_[i], ms);
    }
    ++count_[i];
    sum_[i] += ms;

    auto& s = samples_[i];
    if (s.size() < cfg_.sample_capacity) {
        s.push_back(ms);
    } else {
        // 环形覆盖：样本够多时旧样本的边际价值低，换内存有界
        s[static_cast<size_t>(count_[i] - 1) % cfg_.sample_capacity] = ms;
    }
    if (cfg_.budget_ms[i] > 0 && ms <= double(cfg_.budget_ms[i])) {
        ++within_[i];
    }
}

double LatencyMetrics::percentile(MetricKind kind, double p) const {
    const int i = idx(kind);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& s = samples_[i];
    if (s.empty()) return 0.0;
    std::vector<double> v = s;
    std::sort(v.begin(), v.end());
    if (p <= 0.0) return v.front();
    if (p >= 1.0) return v.back();
    // 最近秩法：小样本下比线性插值稳，且不会插出没测到的值
    size_t i2 = static_cast<size_t>(p * (v.size() - 1) + 0.5);
    if (i2 >= v.size()) i2 = v.size() - 1;
    return v[i2];
}

int64_t LatencyMetrics::count(MetricKind kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_[idx(kind)];
}
double LatencyMetrics::avg(MetricKind kind) const {
    const int i = idx(kind);
    std::lock_guard<std::mutex> lock(mutex_);
    return count_[i] ? sum_[i] / double(count_[i]) : 0.0;
}
double LatencyMetrics::max(MetricKind kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_ms_[idx(kind)];
}
double LatencyMetrics::min(MetricKind kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return min_ms_[idx(kind)];
}
int64_t LatencyMetrics::within_budget(MetricKind kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return within_[idx(kind)];
}
int64_t LatencyMetrics::budget_ms(MetricKind kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cfg_.budget_ms[idx(kind)];
}
double LatencyMetrics::budget_hit_rate(MetricKind kind) const {
    const int i = idx(kind);
    std::lock_guard<std::mutex> lock(mutex_);
    return count_[i] ? double(within_[i]) / double(count_[i]) : 0.0;
}

MetricSnapshot LatencyMetrics::snapshot() const {
    MetricSnapshot s;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < kMetricKindCount; ++i) {
        s.count[i] = count_[i];
        s.sum[i] = sum_[i];
        s.min_ms[i] = min_ms_[i];
        s.max_ms[i] = max_ms_[i];
        s.within_budget[i] = within_[i];
        s.budget_ms[i] = cfg_.budget_ms[i];
    }
    return s;
}

std::string LatencyMetrics::summary_line() const {
    std::ostringstream os;
    os << "Latency summary:";
    for (int i = 0; i < kMetricKindCount; ++i) {
        if (count_[i] == 0) continue;
        const MetricKind k = static_cast<MetricKind>(i);
        os << " " << metric_kind_name(k) << "="
           << static_cast<int>(avg(k)) << "/"          // avg
           << static_cast<int>(percentile(k, 0.99))    // p99
           << "ms(" << budget_hit_rate(k) * 100 << "%<"
           << budget_ms(k) << ")";
    }
    return os.str();
}

void LatencyMetrics::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < kMetricKindCount; ++i) {
        samples_[i].clear();
        count_[i] = 0;
        sum_[i] = 0;
        min_ms_[i] = 0;
        max_ms_[i] = 0;
        within_[i] = 0;
    }
}

// ========== Timer ==========
// 实现全在头文件（Timer 是嵌套类，成员函数可直接内联）。

}  // namespace voice_agent

// src/orchestrator/eou_detector.hpp
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace voice_agent {

// ========== End of Utterance 检测器 ==========
// 双阈值策略：
//   - 快阈值 (fast_ms): 短停，350ms → 快速响应场景
//   - 强制阈值 (force_ms): 长停，900ms → 无论前序如何都判定结束
//   - 最大语句时长 (max_ms): 20s，防止无限说话

struct EOUDetectorConfig {
    int fast_ms = 350;
    int force_ms = 900;
    int max_utterance_ms = 20000;
};

class EOUDetector {
public:
    using Callback = std::function<void(bool is_eou)>;
    using Config = EOUDetectorConfig;

    explicit EOUDetector(Config config = {});
    ~EOUDetector();

    // 不可复制
    EOUDetector(const EOUDetector&) = delete;
    EOUDetector& operator=(const EOUDetector&) = delete;

    // 设置结束判定回调
    void set_callback(Callback cb);

    // 通知语音开始
    void on_speech_start(uint64_t timestamp_us);

    // 通知语音结束（VAD 检测到静音）
    void on_vad_end(uint64_t timestamp_us);

    // 通知有声帧（VAD 检测到语音）
    void on_vad_speech(uint64_t timestamp_us);

    // 重置状态
    void reset();

    // 获取当前等待时长（毫秒）
    int waiting_ms() const;

    bool is_pending() const { return pending_; }

private:
    void evaluate_eou_(uint64_t now_us);
    void start_pending_timer_();

    Config config_;
    Callback callback_;

    std::atomic<bool> pending_{false};

    // 状态
    uint64_t speech_start_us_{0};      // 本次语音开始时间
    uint64_t last_vad_speech_us_{0};   // 最后有声帧时间
    uint64_t last_vad_end_us_{0};      // 最后无声帧开始时间
    bool had_speech_{false};           // 是否有有效语音
    int fast_timer_id_{0};             // 快阈值定时器ID
    int force_timer_id_{0};            // 强制阈值定时器ID
    int max_timer_id_{0};              // 最大时长定时器ID
    int timer_id_counter_{0};          // 递增定时器ID
    mutable std::mutex text_mutex_;    // 保护状态访问（mutable 供 const 方法使用）
};

}  // namespace voice_agent

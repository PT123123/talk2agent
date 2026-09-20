// src/orchestrator/eou_detector.cpp
#include "eou_detector.hpp"
#include "util/log.hpp"
#include <cmath>
#include <cstring>

namespace voice_agent {

EOUDetector::EOUDetector(Config config) : config_(config) {}

EOUDetector::~EOUDetector() {
    reset();
}

void EOUDetector::set_callback(Callback cb) {
    callback_ = std::move(cb);
}

void EOUDetector::on_speech_start(uint64_t timestamp_us) {
    std::lock_guard<std::mutex> lock(text_mutex_);
    speech_start_us_ = timestamp_us;
    last_vad_speech_us_ = timestamp_us;
    last_vad_end_us_ = 0;
    had_speech_ = false;
    LOG_DEBUG("EOU: speech start at {}", timestamp_us);
}

void EOUDetector::on_vad_speech(uint64_t timestamp_us) {
    std::lock_guard<std::mutex> lock(text_mutex_);
    last_vad_speech_us_ = timestamp_us;
    if (!had_speech_) {
        had_speech_ = true;
        LOG_DEBUG("EOU: first speech frame at {}", timestamp_us);
    }
    last_vad_end_us_ = 0;

    // 如果正在等待中，重新评估
    if (pending_) {
        evaluate_eou_(timestamp_us);
    }
}

void EOUDetector::on_vad_end(uint64_t timestamp_us) {
    std::lock_guard<std::mutex> lock(text_mutex_);
    if (last_vad_end_us_ == 0) {
        last_vad_end_us_ = timestamp_us;
        LOG_DEBUG("EOU: VAD end at {}, pending={}", timestamp_us, pending_.load());
    }

    if (!pending_) {
        // 开始评估
        evaluate_eou_(timestamp_us);
    }
}

void EOUDetector::reset() {
    std::lock_guard<std::mutex> lock(text_mutex_);
    pending_ = false;
    speech_start_us_ = 0;
    last_vad_speech_us_ = 0;
    last_vad_end_us_ = 0;
    had_speech_ = false;
    fast_timer_id_ = 0;
    force_timer_id_ = 0;
    max_timer_id_ = 0;
}

int EOUDetector::waiting_ms() const {
    std::lock_guard<std::mutex> lock(text_mutex_);
    if (!pending_ || last_vad_end_us_ == 0) return 0;
    return static_cast<int>((last_vad_speech_us_ - last_vad_end_us_) / 1000);
}

void EOUDetector::evaluate_eou_(uint64_t now_us) {
    if (!had_speech_) return;

    if (last_vad_end_us_ == 0) {
        // 还在说话中，启动 VAD 结束监听
        pending_ = true;
        LOG_DEBUG("EOU: waiting for VAD end, speech_start={}", speech_start_us_);
        return;
    }

    // 计算静音时长
    int64_t silence_ms = static_cast<int64_t>((now_us - last_vad_speech_us_) / 1000);
    int64_t total_ms = static_cast<int64_t>((last_vad_speech_us_ - speech_start_us_) / 1000);

    LOG_DEBUG("EOU: silence={}ms, total={}ms, fast={}, force={}",
              silence_ms, total_ms, config_.fast_ms, config_.force_ms);

    // 强制阈值：无论语句多长，静音超过 force_ms 必定结束
    if (silence_ms >= config_.force_ms) {
        LOG_INFO("EOU: FORCE EOU (silence={}ms >= {}ms)", silence_ms, config_.force_ms);
        pending_ = false;
        if (callback_) callback_(true);
        return;
    }

    // 快阈值：短语句（<1s）时快速响应
    if (total_ms < 1000 && silence_ms >= config_.fast_ms) {
        LOG_INFO("EOU: FAST EOU (short utterance, silence={}ms)", silence_ms);
        pending_ = false;
        if (callback_) callback_(true);
        return;
    }

    // 还在等待中
    pending_ = true;
}

}  // namespace voice_agent

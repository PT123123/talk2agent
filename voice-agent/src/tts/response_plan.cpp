// src/tts/response_plan.cpp
#include "response_plan.hpp"

#include <algorithm>

namespace voice_agent {

void ResponsePlan::append(const std::vector<SpeechSegment>& segs) {
    if (segs.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : segs) {
        slots_.push_back(Slot{s, SegmentState::Pending});
    }
    stats_.appended += segs.size();
}

std::optional<SpeechSegment> ResponsePlan::next_to_synthesize() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (cursor_ < slots_.size()) {
        auto& s = slots_[cursor_];
        if (s.state == SegmentState::Discarded) {
            ++cursor_;
            continue;
        }
        if (s.state == SegmentState::Pending) {
            return s.seg;
        }
        // 已合成但没播：等播放完再取下一个（cursor 由 mark_played 推进）
        return std::nullopt;
    }
    return std::nullopt;
}

size_t ResponsePlan::claim_next(SpeechSegment& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (cursor_ < slots_.size()) {
        auto& s = slots_[cursor_];
        if (s.state == SegmentState::Discarded) {
            ++cursor_;
            continue;
        }
        if (s.state == SegmentState::Pending) {
            out = s.seg;
            s.state = SegmentState::Synthesized;   // 锁内立即占用
            playing_idx_ = static_cast<long long>(cursor_);
            return cursor_;
        }
        // 当前段已占用（合成/播放中）：等 mark_played 推进游标
        return npos;
    }
    return npos;
}

void ResponsePlan::mark_synthesized(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= slots_.size()) return;
    if (slots_[index].state == SegmentState::Discarded) return;
    slots_[index].state = SegmentState::Synthesized;
}

void ResponsePlan::mark_playing(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= slots_.size()) return;
    slots_[index].state = SegmentState::Playing;
    playing_idx_ = static_cast<long long>(index);
}

void ResponsePlan::mark_played(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= slots_.size()) return;
    // 已丢弃的段不算"播过"，但也要推进游标，否则会卡住
    if (slots_[index].state == SegmentState::Discarded) {
        if (cursor_ <= index) cursor_ = index + 1;
        if (playing_idx_ == static_cast<long long>(index)) playing_idx_ = -1;
        return;
    }
    slots_[index].state = SegmentState::Played;
    ++stats_.played;
    if (cursor_ <= index) cursor_ = index + 1;
    if (playing_idx_ == static_cast<long long>(index)) playing_idx_ = -1;
}

size_t ResponsePlan::discard_unplayed_from(size_t from_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (size_t i = from_index; i < slots_.size(); ++i) {
        auto st = slots_[i].state;
        if (st == SegmentState::Played || st == SegmentState::Playing) continue;
        if (st == SegmentState::Discarded) continue;
        slots_[i].state = SegmentState::Discarded;
        ++n;
    }
    if (n > 0) {
        stats_.discarded += n;
        ++stats_.revised_rounds;
    }
    return n;
}

size_t ResponsePlan::discard_all_unplayed() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (auto& s : slots_) {
        if (s.state == SegmentState::Played || s.state == SegmentState::Playing) {
            continue;
        }
        if (s.state == SegmentState::Discarded) continue;
        s.state = SegmentState::Discarded;
        ++n;
    }
    if (n > 0) {
        stats_.discarded += n;
        ++stats_.revised_rounds;
    }
    return n;
}

size_t ResponsePlan::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& s : slots_) {
        if (s.state == SegmentState::Pending || s.state == SegmentState::Synthesized) {
            ++n;
        }
    }
    return n;
}

bool ResponsePlan::finished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : slots_) {
        if (s.state == SegmentState::Pending ||
            s.state == SegmentState::Synthesized ||
            s.state == SegmentState::Playing) {
            return false;
        }
    }
    return true;
}

long long ResponsePlan::playing_index() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_idx_;
}

size_t ResponsePlan::played_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_.played;
}

ResponsePlan::Stats ResponsePlan::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void ResponsePlan::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.clear();
    cursor_ = 0;
    playing_idx_ = -1;
    stats_ = Stats{};
}

}  // namespace voice_agent

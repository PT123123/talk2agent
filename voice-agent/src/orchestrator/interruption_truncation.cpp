// src/orchestrator/interruption_truncation.cpp
#include "interruption_truncation.hpp"

#include <algorithm>
#include <cmath>

namespace voice_agent {

TruncationResult InterruptionTruncation::truncate(const std::string& full_text,
                                                  double played_ratio) const {
    TruncationResult r;
    if (full_text.empty()) return r;

    // 比例合法性：没播过（0）视为全丢；>=1 视为全留
    if (played_ratio < 0.0) played_ratio = 0.0;
    if (played_ratio > 1.0) played_ratio = 1.0;
    r.played_ratio = played_ratio;

    // 全播完了 —— 不是截断场景
    if (played_ratio >= 0.999) {
        r.played_text = full_text;
        return r;
    }

    r.truncated = true;

    if (played_ratio < cfg_.min_played_ratio) {
        // 几乎没播出 —— 整段丢弃，只留标记
        r.played_text.clear();
        r.dropped_text = full_text;
        if (cfg_.append_marker) r.played_text = cfg_.interrupt_marker;
        return r;
    }

    // 按比例切分。切点对齐到最近的句边界（避免把句子切成两半）。
    const size_t keep = static_cast<size_t>(
        std::lround(static_cast<double>(full_text.size()) * played_ratio));
    size_t cut = std::min(keep, full_text.size());

    // 向前找最近的句读（。！？；\n），避免半个句子
    static const std::string kBreaks = "。！？；!?;\n";
    size_t best = std::string::npos;
    for (size_t i = 0; i < cut && i < full_text.size(); ++i) {
        if (kBreaks.find(full_text[i]) != std::string::npos) {
            best = i;
        }
    }
    if (best != std::string::npos && best + 1 <= keep + kBreaks.size()) {
        cut = best + 1;
    }
    // 句边界太靠前（超过一半）就用原始比例点，宁可精确不要丢太多内容
    if (cut < keep / 2) cut = std::min(keep, full_text.size());

    r.played_text = full_text.substr(0, cut);
    r.dropped_text = full_text.substr(cut);
    if (cfg_.append_marker && !r.played_text.empty()) {
        r.played_text += cfg_.interrupt_marker;
    }
    return r;
}

TruncationResult InterruptionTruncation::truncate_by_segments(
    const std::vector<std::string>& played_segments,
    const std::vector<std::string>& all_segments) const {
    TruncationResult r;
    if (all_segments.empty()) return r;

    for (const auto& s : played_segments) {
        if (!r.played_text.empty()) r.played_text += "\n";
        r.played_text += s;
    }
    for (size_t i = played_segments.size(); i < all_segments.size(); ++i) {
        if (!r.dropped_text.empty()) r.dropped_text += "\n";
        r.dropped_text += all_segments[i];
    }

    r.played_ratio = static_cast<double>(played_segments.size()) /
                     static_cast<double>(all_segments.size());
    r.truncated = played_segments.size() < all_segments.size();

    if (r.truncated && cfg_.append_marker) {
        if (r.played_text.empty()) {
            r.played_text = cfg_.interrupt_marker;
        } else {
            r.played_text += cfg_.interrupt_marker;
        }
    }
    return r;
}

std::string InterruptionTruncation::build_interrupt_note(
    const TruncationResult& r) const {
    if (!r.truncated) return {};
    std::string note = "[上一轮被用户打断]";
    if (!r.dropped_text.empty()) {
        note += " 你原本要说但没说完的内容：" + r.dropped_text;
    }
    note += " 不要假设用户听到了那些内容；可以自然地承认被打断，或直接回应新问题。";
    return note;
}

}  // namespace voice_agent

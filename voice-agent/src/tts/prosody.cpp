// src/tts/prosody.cpp
#include "prosody.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace voice_agent {

// ========== Prosody ==========

bool Prosody::operator==(const Prosody& o) const {
    auto eq = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    return eq(energy, o.energy) && eq(warmth, o.warmth) &&
           eq(certainty, o.certainty) && eq(urgency, o.urgency) &&
           eq(pace, o.pace) && eq(pause_density, o.pause_density);
}

Prosody prosody_for_emotion(const std::string& emotion, Prosody base) {
    if (emotion.empty()) return base;

    // 情绪 → 韵律偏移量。刻意做小幅调整：情绪是"染色"不是"换人"，
    // 幅度太大会导致每句话音色突变，反而更像机器人。
    Prosody d;
    if (emotion == "happy" || emotion == "excited" || emotion == "开心" ||
        emotion == "兴奋") {
        d.energy = +0.18f;  d.warmth = +0.14f;  d.pace = +0.14f;
        d.certainty = +0.05f; d.pause_density = -0.10f; d.urgency = +0.10f;
    } else if (emotion == "calm" || emotion == "平静" || emotion == "gentle") {
        d.energy = -0.12f;  d.warmth = +0.12f;  d.pace = -0.12f;
        d.pause_density = +0.10f; d.urgency = -0.10f;
    } else if (emotion == "sad" || emotion == "难过" || emotion == "低落") {
        d.energy = -0.20f;  d.warmth = +0.08f;  d.pace = -0.18f;
        d.pause_density = +0.16f; d.certainty = -0.08f;
    } else if (emotion == "worried" || emotion == "concerned" ||
               emotion == "担心") {
        d.energy = -0.08f;  d.warmth = +0.10f;  d.urgency = +0.18f;
        d.certainty = -0.14f; d.pause_density = +0.08f;
    } else if (emotion == "confused" || emotion == "疑惑") {
        // 疑惑：上扬的语调 —— warmth 低、certainty 低、停顿多
        d.warmth = -0.06f;  d.certainty = -0.20f;
        d.pause_density = +0.18f; d.pace = -0.06f;
    } else if (emotion == "user_irritated" || emotion == "annoyed" ||
               emotion == "用户不耐烦") {
        // 关键：用户已经烦了，不能再用"热情"去回应。
        // 应该是 energy↓ pace↓ warmth↑ —— 稳住，别再刺激。
        d.energy = -0.16f;  d.pace = -0.20f;  d.warmth = +0.06f;
        d.urgency = -0.10f; d.certainty = +0.10f;
    } else if (emotion == "apologetic" || emotion == "抱歉") {
        d.energy = -0.14f;  d.warmth = +0.14f;  d.certainty = -0.12f;
        d.pace = -0.10f;
    } else {
        return base;   // 未知情绪：不动
    }

    Prosody out;
    out.energy      = std::clamp(base.energy + d.energy, 0.0f, 1.0f);
    out.warmth      = std::clamp(base.warmth + d.warmth, 0.0f, 1.0f);
    out.certainty   = std::clamp(base.certainty + d.certainty, 0.0f, 1.0f);
    out.urgency     = std::clamp(base.urgency + d.urgency, 0.0f, 1.0f);
    out.pace        = std::clamp(base.pace + d.pace, 0.0f, 1.0f);
    out.pause_density = std::clamp(base.pause_density + d.pause_density, 0.0f, 1.0f);
    return out;
}

// ========== 句级切分 ==========
std::vector<std::string> ProsodyPlanner::split_sentences(const std::string& text,
                                                         int max_chars) {
    std::vector<std::string> out;
    if (text.empty()) return out;

    // 主标点（中英）。找到这些位置就是句子边界。
    static const std::string kTerminal = "。！？；!?;\n";
    // 次级标点：超过 max_chars 时用它们再切
    static const std::string kSecondary = "，、：,:";

    size_t start = 0;
    while (start < text.size()) {
        // 找下一个主标点
        size_t cut = std::string::npos;
        for (size_t i = start; i < text.size(); ++i) {
            if (kTerminal.find(text[i]) != std::string::npos) {
                cut = i;
                break;
            }
        }

        if (cut == std::string::npos) {
            // 没有主标点：整段（或按次级标点切）
            const size_t len = text.size() - start;
            if (len <= static_cast<size_t>(max_chars)) {
                out.push_back(text.substr(start));
                break;
            }
            size_t sec = std::string::npos;
            for (size_t i = start; i < text.size(); ++i) {
                if (kSecondary.find(text[i]) != std::string::npos) {
                    sec = i;
                    break;
                }
            }
            if (sec == std::string::npos || sec - start < static_cast<size_t>(max_chars) / 2) {
                out.push_back(text.substr(start, static_cast<size_t>(max_chars)));
                start += static_cast<size_t>(max_chars);
            } else {
                out.push_back(text.substr(start, sec - start + 1));
                start = sec + 1;
            }
            continue;
        }

        std::string seg = text.substr(start, cut - start + 1);

        // 连续标点（如 "？！"）应该并入同一段
        size_t end = cut + 1;
        while (end < text.size() &&
               (kTerminal.find(text[end]) != std::string::npos)) {
            seg += text[end];
            ++end;
        }
        out.push_back(std::move(seg));
        start = end;
    }

    // 丢弃纯标点/空白的段
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& s) {
        for (unsigned char c : s) {
            if (c > 0x7F) return false;                       // 含中文等即认为有内容
            if (!std::isspace(c) &&
                std::string("。！？；：、,.!?;:()[]{}《》“”‘’\"'-").find(
                    static_cast<char>(c)) == std::string::npos) {
                return false;
            }
        }
        return true;
    }), out.end());

    return out;
}

// ========== 韵律规划 ==========
SpeechSegment ProsodyPlanner::plan_segment(const std::string& sentence,
                                            const std::string& emotion,
                                            float intensity) const {
    SpeechSegment seg;
    seg.text = sentence;

    Prosody base;   // 中性基线
    base.energy = 0.5f;
    base.warmth = 0.5f;
    base.certainty = 0.6f;
    base.urgency = 0.3f;
    base.pace = 0.5f;
    base.pause_density = 0.5f;

    if (!emotion.empty() && intensity > 0.0f) {
        Prosody shifted = prosody_for_emotion(emotion, base);
        // 按 intensity 在"中性"与"情绪"之间插值。
        // 强度不足时完全保持中性 —— 半吊子的情绪比没有情绪更像机器人。
        seg.prosody.energy = base.energy + (shifted.energy - base.energy) * intensity;
        seg.prosody.warmth = base.warmth + (shifted.warmth - base.warmth) * intensity;
        seg.prosody.certainty =
            base.certainty + (shifted.certainty - base.certainty) * intensity;
        seg.prosody.urgency =
            base.urgency + (shifted.urgency - base.urgency) * intensity;
        seg.prosody.pace = base.pace + (shifted.pace - base.pace) * intensity;
        seg.prosody.pause_density =
            base.pause_density + (shifted.pause_density - base.pause_density) * intensity;
    } else {
        seg.prosody = base;
    }

    seg.pause_after_ms = pause_after_ms_from_prosody(seg.prosody);
    seg.estimated_ms = estimate_duration_ms(sentence, seg.prosody);
    return seg;
}

std::vector<SpeechSegment> ProsodyPlanner::plan(const std::string& text,
                                                const std::string& emotion,
                                                float intensity,
                                                bool is_turn_head,
                                                bool is_turn_tail) const {
    std::vector<SpeechSegment> segs;
    if (text.empty()) return segs;

    auto sents = split_sentences(text, cfg_.max_segment_chars);
    for (size_t i = 0; i < sents.size(); ++i) {
        auto seg = plan_segment(sents[i], emotion, intensity);

        // 轮次开头：稍作停顿再开口，让用户确认"这是在回答我"
        if (i == 0 && is_turn_head) {
            seg.pause_before_ms = 60;
        }
        // 轮次末尾：留白。不留白会让下一句"我明白了"紧贴上来，
        // 听起来像连着说了两件事而不是一个回答结束。
        if (i + 1 == sents.size() && is_turn_tail) {
            seg.pause_after_ms = std::max(seg.pause_after_ms, 180);
        }
        segs.push_back(std::move(seg));
    }
    return segs;
}

int ProsodyPlanner::estimate_duration_ms(const std::string& text, Prosody p) const {
    // 按字符数估时长：中文约 5-7 字/秒，英文约 12-15 字符/秒。
    size_t cjk = 0, ascii = 0;
    for (unsigned char c : text) {
        if (c > 0x7F) ++cjk;
        else if (!std::isspace(c)) ++ascii;
    }
    float cps_cjk = cfg_.base_cps * (0.7f + 0.6f * p.pace);
    float cps_ascii = cfg_.base_cps * 2.2f * (0.7f + 0.6f * p.pace);
    const float seconds = static_cast<float>(cjk) / cps_cjk +
                          static_cast<float>(ascii) / cps_ascii;
    return static_cast<int>(seconds * 1000.0f) +
           static_cast<int>(p.pause_density * 120.0f);
}

float ProsodyPlanner::rate_from_prosody(Prosody p) {
    // pace 与 urgency 共同决定语速。certainty 低会略微放慢（迟疑感）。
    float rate = 0.75f + 0.45f * p.pace + 0.15f * p.urgency - 0.10f * (1.0f - p.certainty);
    return std::clamp(rate, 0.6f, 1.8f);
}

int ProsodyPlanner::pause_after_ms_from_prosody(Prosody p) {
    // pause_density 高 + certainty 低 => 停顿更明显（不确定的话迟疑更久）
    float ms = 60.0f + 260.0f * p.pause_density + 120.0f * (1.0f - p.certainty);
    return static_cast<int>(std::clamp(ms, 0.0f, 520.0f));
}

}  // namespace voice_agent

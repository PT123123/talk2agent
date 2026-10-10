// src/tts/prosody.cpp
#include "prosody.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <utility>

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
namespace {

// 解析 s[pos] 起的一个 UTF-8 码点，cp 输出码点值。
// 返回消耗的字节数；返回 0 表示输入在码点中间被截断（流式累积缓冲的
// 常态：token 可能把一个汉字劈成两半）—— 调用方应停止扫描，把剩余
// 字节留在缓冲里等下一批数据补全。非法字节按 1 字节消耗、cp=0xFFFD。
size_t utf8_next(const std::string& s, size_t pos, char32_t& cp) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80) { cp = static_cast<char32_t>(c); return 1; }
    size_t need; char32_t acc;
    if ((c & 0xE0) == 0xC0)      { need = 2; acc = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { need = 3; acc = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { need = 4; acc = c & 0x07u; }
    else { cp = 0xFFFD; return 1; }
    if (pos + need > s.size()) return 0;   // 截断：等更多数据
    for (size_t k = 1; k < need; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[pos + k]);
        if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; return 1; }
        acc = (acc << 6) | (cc & 0x3Fu);
    }
    cp = acc;
    return need;
}

// 标点匹配必须比较完整 UTF-8 序列，绝不能按单字节找 ——
// "呀"(E5 91 80)、"怎"(E6 80 8E)、"一"(E4 B8 80) 的编码字节与
// "。"(E3 80 82) 重叠，按字节匹配会把汉字切碎。
bool is_terminal_seq(const std::string& s, size_t pos, size_t len) {
    static const char* const kSeqs[] = {
        "。", "！", "？", "；", "…", "!", "?", ";", "\n",
    };
    for (const char* p : kSeqs) {
        if (std::strlen(p) == len && s.compare(pos, len, p) == 0) return true;
    }
    return false;
}

// 次级标点：只在段内过半/超长时作为切分点，平时不切
bool is_secondary_seq(const std::string& s, size_t pos, size_t len) {
    static const char* const kSeqs[] = { "，", "、", "：", ",", ":" };
    for (const char* p : kSeqs) {
        if (std::strlen(p) == len && s.compare(pos, len, p) == 0) return true;
    }
    return false;
}

// 段内是否含可念的内容（排除空白与标点；尾部截断按有内容处理）
bool seg_has_speech(const std::string& s) {
    static const char* const kPunct[] = {
        "，", "。", "！", "？", "；", "：", "、", "…", "—", "·",
        "（", "）", "《", "》", "“", "”", "‘", "’",
        ",", ".", "!", "?", ";", ":",
        "(", ")", "[", "]", "{", "}", "\"", "'", "-",
    };
    size_t i = 0;
    while (i < s.size()) {
        char32_t cp;
        const size_t n = utf8_next(s, i, cp);
        if (n == 0) return true;    // 尾部截断：等补全，先当有内容
        bool punct = false;
        for (const char* p : kPunct) {
            if (std::strlen(p) == n && s.compare(i, n, p) == 0) { punct = true; break; }
        }
        if (!punct) {
            if (n == 1 && std::isspace(static_cast<unsigned char>(s[i]))) {
                i += n;
                continue;
            }
            return true;            // 中文/字母/数字等实际内容
        }
        i += n;
    }
    return false;
}

}  // namespace

std::vector<std::string> ProsodyPlanner::split_sentences(const std::string& text,
                                                         int max_chars,
                                                         bool hold_tail) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    const size_t max_cp = max_chars > 0 ? static_cast<size_t>(max_chars) : 120;

    std::string cur;          // 当前句累积（UTF-8）
    size_t cur_chars = 0;     // 当前句码点数
    auto flush = [&]() {
        if (!cur.empty()) out.push_back(std::move(cur));
        cur.clear();
        cur_chars = 0;
    };

    size_t i = 0;
    while (i < text.size()) {
        char32_t cp;
        const size_t n = utf8_next(text, i, cp);
        if (n == 0) break;    // 尾部不完整码点：留在缓冲等补全

        cur.append(text, i, n);
        ++cur_chars;
        i += n;

        if (is_terminal_seq(text, i - n, n)) {
            // 连续终止标点并入同段（"？！"、"。。。"）
            while (i < text.size()) {
                char32_t cp2;
                const size_t n2 = utf8_next(text, i, cp2);
                if (n2 == 0 || !is_terminal_seq(text, i, n2)) break;
                cur.append(text, i, n2);
                i += n2;
            }
            flush();
            continue;
        }

        // 超长无标点：次级标点过半后可切，到硬上限必须切
        //（码点边界，绝不切坏 UTF-8）
        if (cur_chars >= max_cp ||
            (cur_chars * 2 >= max_cp && is_secondary_seq(text, i - n, n))) {
            flush();
        }
    }

    if (!hold_tail && !cur.empty()) {
        // 整段规划模式：无终止标点的尾巴也作为最后一段返回
        out.push_back(std::move(cur));
    }
    // hold_tail=true：尾巴留在输入缓冲里，feed 只消费返回的字节

    // 丢弃纯标点/空白的段
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const std::string& s) { return !seg_has_speech(s); }),
              out.end());
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

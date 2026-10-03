// src/orchestrator/semantic_turn.cpp
#include "semantic_turn.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace voice_agent {

namespace {

// 结尾连接词/助词：出现这些说明话还没说完
const std::vector<std::string>& trailing_connectors() {
    static const std::vector<std::string> v = {
        // 中文
        "那个", "这个", "就是", "然后", "因为", "所以", "但是", "可是",
        "如果", "要是", "比如", "例如", "以及", "或者", "而且", "并且",
        "比如", "关于", "对于", "至于", "嗯", "呃", "啊", "哦",
        "我", "你", "他", "她", "它", "咱", "俺",
        // 英文
        "and", "but", "or", "so", "because", "if", "when", "which",
        "that", "the", "a", "an", "um", "uh", "er",
    };
    return v;
}

// 句末标点：强完成信号
bool ends_with_terminal(const std::string& s) {
    if (s.empty()) return false;
    static const std::string kTerminal = "。！？!?…";
    const char c = s.back();
    return kTerminal.find(c) != std::string::npos;
}

// 疑问/反问句也是完成信号
bool has_question_mark(const std::string& s) {
    return s.find('？') != std::string::npos ||
           s.find('?') != std::string::npos;
}

// 结尾是逗号/顿号 —— 半句，最不该抢话
bool ends_with_comma(const std::string& s) {
    if (s.empty()) return false;
    const char c = s.back();
    return c == ',' || c == '，' || c == '、' || c == ':' || c == '：';
}

size_t char_count(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;   // UTF-8 续字节不计
    }
    return n;
}

std::string normalized(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    // 去掉尾部空白
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    return out;
}

bool ends_with_connector(const std::string& s) {
    for (const auto& w : trailing_connectors()) {
        const size_t wl = char_count(w);
        if (s.size() < wl) continue;
        // 取尾部 wl 个"字符"（UTF-8）
        size_t start = s.size();
        size_t count = 0;
        size_t i = s.size();
        while (i > 0 && count < wl) {
            --i;
            while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
            ++count;
        }
        if (count < wl) continue;
        if (normalized(s.substr(i)) == w) return true;
    }
    return false;
}

}  // namespace

CompletionHint SemanticTurnDetector::analyze(const std::string& raw) const {
    const std::string text = normalized(raw);
    if (text.empty()) return CompletionHint::Incomplete;

    // 极短 utterance（"嗯"、"好"、"OK"）—— 直接算说完。
    // 这些几乎总是附和而非新问题，语义分析在这里没有意义。
    if (char_count(text) <= static_cast<size_t>(cfg_.short_utterance_chars)) {
        if (ends_with_comma(text)) return CompletionHint::Incomplete;
        return CompletionHint::LikelyDone;
    }

    // 结尾是逗号/顿号/冒号 —— 铁定没说完
    if (ends_with_comma(text)) return CompletionHint::Incomplete;

    // 结尾是省略号 —— 说话人在犹豫
    if (text.size() >= 3 && text.substr(text.size() - 3) == "...") {
        return CompletionHint::Incomplete;
    }

    // 疑问句：用户问完了，在等回答
    if (has_question_mark(text)) return CompletionHint::LikelyDone;

    // 句末标点：强完成信号
    if (ends_with_terminal(text)) {
        // 但"因为…"、"就是…"这种以标点结尾的连接句仍可能没说完
        if (ends_with_connector(text)) return CompletionHint::Uncertain;
        return CompletionHint::LikelyDone;
    }

    // 结尾是连接词 —— 明显没说完
    if (ends_with_connector(text)) return CompletionHint::Incomplete;

    // 无标点无连接词：保守起见判不确定
    return CompletionHint::Uncertain;
}

bool SemanticTurnDetector::should_yield(const std::string& text,
                                        int silence_ms) const {
    const CompletionHint hint = analyze(text);
    const EagernessConfig ec = eagerness_config(cfg_.eagerness);

    Decision d;
    d.hint = hint;
    d.waited_ms = silence_ms;

    int threshold = ec.min_wait_ms;
    switch (hint) {
        case CompletionHint::LikelyDone:
            // 语义完整 —— 但仍要给一个最小等待，让用户有机会改口。
            d.yield = silence_ms >= ec.min_wait_ms;
            d.reason = "semantic=likely_done, wait>=" +
                       std::to_string(ec.min_wait_ms) + "ms";
            break;

        case CompletionHint::Incomplete:
            // 明显没说完 —— 静默再久也不该抢话（直到上限兜底，
            // 否则用户彻底停顿时 Agent 会永远沉默）。
            threshold = ec.min_wait_ms + ec.doubt_bonus_ms;
            d.yield = silence_ms >= std::min(ec.max_wait_ms, threshold);
            d.reason = "semantic=incomplete, wait>=" + std::to_string(threshold) +
                       "ms (capped at " + std::to_string(ec.max_wait_ms) + ")";
            break;

        case CompletionHint::Uncertain:
            threshold = ec.min_wait_ms + ec.doubt_bonus_ms / 2;
            d.yield = silence_ms >= threshold;
            d.reason = "semantic=uncertain, wait>=" + std::to_string(threshold) +
                       "ms";
            break;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = d;
    }
    return d.yield;
}

SemanticTurnDetector::Decision SemanticTurnDetector::last_decision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

}  // namespace voice_agent

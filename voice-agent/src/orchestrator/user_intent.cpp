// src/orchestrator/user_intent.cpp
#include "user_intent.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace voice_agent {

namespace {

// 粗略字符数：按 UTF-8 码点计（中文 1 个字算 1 字符，而不是 3 字节）。
// 直接用 strlen 会让中文句子长度虚高 3 倍，长度闸门就失效了。
size_t count_utf8ish(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;   // 跳过 UTF-8 续字节
    }
    return n;
}

bool in_list(const std::vector<std::string>& list, const std::string& s) {
    for (const auto& w : list) {
        if (w == s) return true;
    }
    return false;
}

bool starts_with_any(const std::string& s, const std::vector<std::string>& list) {
    for (const auto& w : list) {
        if (s.size() >= w.size() && s.compare(0, w.size(), w) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

const std::vector<std::string>& UserIntentClassifier::backchannel_words() {
    static const std::vector<std::string> w = {
        // 中文
        "嗯", "嗯嗯", "恩", "唔", "哦", "噢", "喔", "啊", "诶",
        "对", "对的", "对对", "对的对", "是", "是的", "是啊", "没错", "正确",
        "好", "好的", "好吧", "行", "行吧", "可以", "嗯好", "好好好",
        "收到", "明白", "懂了", "知道了", "了解",
        // 英文
        "yeah", "yep", "yup", "yes", "ok", "okay", "mm", "mm hmm", "mhm",
        "uh huh", "uh-huh", "right", "sure", "got it", "i see", "true",
        "hmm", "hm", "ah", "oh", "alright", "exactly", "correct", "indeed",
    };
    return w;
}

const std::vector<std::string>& UserIntentClassifier::continuation_words() {
    static const std::vector<std::string> w = {
        "继续", "接着说", "然后呢", "然后", "接着", "再说", "下一个",
        "还有呢", "就这些", "继续讲", "继续说", "讲完", "往下说",
        "go on", "continue", "keep going", "next", "and then", "anything else",
    };
    return w;
}

const std::vector<std::string>& UserIntentClassifier::correction_words() {
    static const std::vector<std::string> w = {
        "不对", "不是", "不是的", "错了", "我说错了", "搞错了",
        "更正", "纠正一下", "应该是", "改成", "应该是这样",
        "no", "wrong", "incorrect", "i mean", "actually no", "correction",
    };
    return w;
}

const std::vector<std::string>& UserIntentClassifier::interruption_words() {
    static const std::vector<std::string> w = {
        "等一下", "等等", "停", "停一下", "先别说了", "别说了", "先别说",
        "打断一下", "稍等", "先等等", "别讲", "安静", "停停",
        "wait", "hold on", "stop", "hang on", "shut up", "be quiet",
    };
    return w;
}

const std::vector<std::string>& UserIntentClassifier::topic_change_words() {
    static const std::vector<std::string> w = {
        "算了", "换个话题", "另一个事", "另外一件事", "别的了",
        "对了", "顺便问一下", "顺便问", "还有个事", "换个问题",
        "不说这个了", "先不说这个", "另一个问题", "问一下别的",
        "actually", "by the way", "btw", "never mind", "nevermind",
        "forget it", "different question", "something else", "another thing",
        "on second thought",
    };
    return w;
}

std::string UserIntentClassifier::normalize(const std::string& text) {
    // 去首尾空白
    size_t b = 0, e = text.size();
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (b < e && is_ws(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && is_ws(static_cast<unsigned char>(text[e - 1]))) --e;
    std::string s = text.substr(b, e - b);

    // 去首尾常见中英文标点与语气助词残留（。？！，、~ … -—)
    const std::string trim_chars = "。？！?!.,，、~～… \t";
    size_t b2 = 0, e2 = s.size();
    while (b2 < e2 && trim_chars.find(s[b2]) != std::string::npos) ++b2;
    while (e2 > b2 && trim_chars.find(s[e2 - 1]) != std::string::npos) --e2;
    s = s.substr(b2, e2 - b2);

    // 英文小写化（中文不受影响）
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

UserSpeechIntent UserIntentClassifier::classify(const std::string& raw) const {
    std::string s = normalize(raw);
    if (s.empty()) return UserSpeechIntent::Backchannel;  // 空语音段按附和处理

    // 长度闸门：只对极短的句子允许判为 Backchannel。
    // 这条比词表更重要 —— "嗯对了帮我查一下 Qwen3" 必须判为 Content。
    const bool short_enough = count_utf8ish(s) <= cfg_.max_backchannel_chars;

    if (short_enough && in_list(backchannel_words(), s)) {
        return UserSpeechIntent::Backchannel;
    }
    // 打断/换话题即使带后缀也要能识别（"等一下，我查个东西"），
    // 所以这两类不受长度闸门限制，但要求出现在句首。
    if (starts_with_any(s, interruption_words())) {
        return UserSpeechIntent::Interruption;
    }
    if (starts_with_any(s, topic_change_words())) {
        return UserSpeechIntent::TopicChange;
    }
    if (short_enough && in_list(continuation_words(), s)) {
        return UserSpeechIntent::Continuation;
    }
    if (starts_with_any(s, correction_words())) {
        return UserSpeechIntent::Correction;
    }
    return UserSpeechIntent::Content;
}

bool UserIntentClassifier::should_yield_turn(const std::string& raw) const {
    switch (classify(raw)) {
        case UserSpeechIntent::Backchannel:  return false;  // 不让出，Agent 继续
        case UserSpeechIntent::Interruption: return true;   // 立刻 barge-in
        case UserSpeechIntent::TopicChange:  return true;
        case UserSpeechIntent::Correction:   return true;
        case UserSpeechIntent::Continuation: return false;  // 等它说完
        case UserSpeechIntent::Content:      return true;
    }
    return true;
}

}  // namespace voice_agent

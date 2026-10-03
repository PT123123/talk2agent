// src/orchestrator/user_intent.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace voice_agent {

// ========== 用户话语意图 ==========
// 把"用户说了什么"和"用户想干什么"分开建模。
// 散落在各处的 if (text == "嗯") 正是自然度杀手 —— 这里给出正式枚举。
enum class UserSpeechIntent {
    Content,        // 实质内容：正常的新信息/提问
    Backchannel,    // 附和："嗯""对""好""是的""OK" —— 不应打断 Agent
    Continuation,   // 续说："继续""然后呢""接着说"
    Correction,     // 纠正："不是的""我说错了""应该改成"
    Interruption,   // 打断："等一下""停""先别说了"
    TopicChange     // 换话题："算了""另一个事""对了，顺便问一下"
};

inline const char* user_speech_intent_to_string(UserSpeechIntent i) {
    switch (i) {
        case UserSpeechIntent::Content:       return "Content";
        case UserSpeechIntent::Backchannel:   return "Backchannel";
        case UserSpeechIntent::Continuation:  return "Continuation";
        case UserSpeechIntent::Correction:    return "Correction";
        case UserSpeechIntent::Interruption:  return "Interruption";
        case UserSpeechIntent::TopicChange:   return "TopicChange";
    }
    return "Unknown";
}

// ========== 话语分类器 ==========
// 纯规则 + 词表，零模型开销。词表刻意做得保守：
// 宁可漏判（当作 Content，走原有时长判定）也不误判成 Backchannel
// 把用户的正常发言吃掉。
class UserIntentClassifier {
public:
    struct Config {
        // 附和词最大字符数。超过这个长度一定不是 backchannel
        // （"嗯我查一下最新的 Qwen3" 里的"嗯"不能当成附和）。
        size_t max_backchannel_chars{8};
        // 是否启用英文词表
        bool enable_en{true};
    };

    explicit UserIntentClassifier(Config cfg = {}) : cfg_(cfg) {}

    // 主入口。text 为 ASR 结果（已 trim + 小写化处理由 classify 内部做）
    UserSpeechIntent classify(const std::string& text) const;

    // 便捷判定
    bool is_backchannel(const std::string& text) const {
        return classify(text) == UserSpeechIntent::Backchannel;
    }
    bool is_interruption(const std::string& text) const {
        return classify(text) == UserSpeechIntent::Interruption;
    }
    bool is_topic_change(const std::string& text) const {
        return classify(text) == UserSpeechIntent::TopicChange;
    }

    // Agent 正在说话时，用户这句话是否应该立刻让出话轮。
    // 与 classify 分离：时长类信号由 SmartTurn 负责，这里只管语义。
    bool should_yield_turn(const std::string& text) const;

    static const std::vector<std::string>& backchannel_words();
    static const std::vector<std::string>& continuation_words();
    static const std::vector<std::string>& correction_words();
    static const std::vector<std::string>& interruption_words();
    static const std::vector<std::string>& topic_change_words();

    // 归一化：去首尾空白 + 常见标点 + 转小写
    static std::string normalize(const std::string& text);

private:
    Config cfg_;
};

}  // namespace voice_agent

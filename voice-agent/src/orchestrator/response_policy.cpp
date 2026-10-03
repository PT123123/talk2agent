// src/orchestrator/response_policy.cpp
#include "response_policy.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace voice_agent {

namespace {

std::string norm(const std::string& s) {
    size_t b = 0, e = s.size();
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (b < e && is_ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool has_any(const std::string& hay, std::initializer_list<const char*> needles) {
    for (auto n : needles) {
        if (has(hay, n)) return true;
    }
    return false;
}

// 情感倾诉 / 闲聊：不需要任何工具，就是聊天。
// 必须排在时效性判定之前 —— "我最近真的有好多事情"里的"最近"是时间副词，
// 不是查询限定词，误判成 Search 会去查"最近"然后拿一堆无关结果回来。
bool looks_like_emotional_smalltalk(const std::string& s) {
    static const std::vector<std::string> kEmotion = {
        "我最近", "我今天", "我最近真的", "最近好多", "最近有点", "最近挺",
        "好累", "好烦", "很累", "心情", "难受", "开心", "高兴", "难过",
        "压力大", "焦虑", "失眠", "孤独", "郁闷", "爽", "舒服", "糟心",
        "tired", "exhausted", "stressed", "anxious", "sad", "happy", "lonely",
    };
    for (const auto& e : kEmotion) {
        if (s.find(e) != std::string::npos) return true;
    }

    // 纯主观陈述（无疑问词、无请求动词）也算倾诉
    const bool has_question = s.find('？') != std::string::npos ||
                              s.find('?') != std::string::npos ||
                              has_any(s, {"怎么", "如何", "为什么", "什么", "哪些",
                                          "吗", "呢", "?", "？", "how", "what", "why"});
    const bool has_request = has_any(s, {"帮我", "查一下", "看一下", "找一下", "搜",
                                        "打开", "运行", "帮我查"});
    return !has_question && !has_request;
}

// 需要实时事实的信号词
bool looks_like_freshness_query(const std::string& s) {
    return has_any(s, {
        // 中文时效性
        "最新", "最近", "现在", "目前", "当前", "今天", "现在几点",
        "多少钱", "价格", "股价", "版本", "更新", "发布了", "怎么样",
        "如何了", "进展", "消息", "新闻", "动态",
        // 英文
        "latest", "recent", "now", "current", "today", "price", "cost",
        "how much", "news", "update", "release", "version", "status",
    });
}

// 需要操作本地（文件/命令/项目）
bool looks_like_local_action(const std::string& s) {
    return has_any(s, {
        "文件", "文件夹", "目录", "路径", "项目", "代码", "仓库", "编译",
        "构建", "运行", "执行", "命令", "脚本", "打开", "保存", "修改",
        "删除", "复制", "移动", "查找", "搜索一下我的", "帮我改",
        "file", "folder", "directory", "path", "project", "code", "repo",
        "build", "compile", "run", "execute", "command", "script", "open",
        "save", "edit", "delete", "copy", "move", "grep",
    });
}

// 需要个人上下文（长期记忆）
bool looks_like_personal_context(const std::string& s) {
    return has_any(s, {
        "我的", "我记得", "之前", "上次", "上次你", "你还记得", "我是谁",
        "我叫什么", "我住", "我的偏好", "我之前", "咱们", "我们之前",
        "my ", "you remember", "last time", "previously", "who am i",
        "my name", "i told you", "earlier",
    });
}

// 简单问题：可以直接快速答，不必动用大模型
bool looks_trivial(const std::string& s) {
    if (s.size() > 24) return false;   // 长句不算 trivial
    return has_any(s, {
        "几点", "什么时间", "今天几号", "星期几", "现在几点",
        "你好", "在吗", "谢谢", "再见", "好的", "嗯",
        "what time", "what's the time", "hello", "hi ", "thanks", "bye",
    });
}

// 复杂推理
bool looks_like_deep_reasoning(const std::string& s) {
    return has_any(s, {
        "为什么", "怎么样才能", "如何设计", "架构", "方案", "对比", "比较",
        "区别", "优缺点", "利弊", "权衡", "证明", "推导", "分析一下",
        "评估", "规划", "策略", "该怎么选", "值不值得", "风险",
        "why", "design", "architect", "compare", "trade-off", "tradeoff",
        "pros and cons", "analyze", "evaluate", "plan", "strategy",
        "prove", "derive",
    });
}

}  // namespace

// ========== ResponsePolicy ==========

ResponseDecision ResponsePolicy::decide(const ResponsePolicyInput& in) const {
    ResponseDecision d;
    const std::string text = norm(in.user_text);
    d.latency_budget_ms = cfg_.default_latency_budget_ms;

    // ---- 规则 0：用户话语意图优先于一切 ----
    switch (in.intent) {
        case UserSpeechIntent::Backchannel:
            d.action = ResponseAction::Silence;
            d.tier = ModelTier::Fast;
            d.reason = "user backchannel - stay quiet, keep speaking";
            return d;
        case UserSpeechIntent::Interruption:
            // 用户明确要求停下：不抢话，立刻让出，把控制权交还
            d.action = ResponseAction::Silence;
            d.tier = ModelTier::Fast;
            d.allow_background = true;
            d.reason = "user interrupted - yield immediately";
            return d;
        case UserSpeechIntent::Continuation:
            d.action = ResponseAction::Answer;
            d.depth = ResponseDepth::Low;
            d.tier = ModelTier::Normal;
            d.reason = "user asked to continue";
            return d;
        case UserSpeechIntent::TopicChange:
            d.needs_memory = false;   // 新话题不该被旧话题污染
            d.reason = "topic changed";
            break;
        case UserSpeechIntent::Correction:
            d.depth = ResponseDepth::Medium;
            d.reason = "user corrected - re-answer";
            break;
        case UserSpeechIntent::Content:
            break;
    }

    // ---- 规则 1：Agent 正在说话时不抢话（除非是打断/换话题）----
    if (in.agent_is_speaking && cfg_.never_talk_over_agent &&
        in.intent != UserSpeechIntent::Interruption &&
        in.intent != UserSpeechIntent::TopicChange &&
        in.intent != UserSpeechIntent::Correction) {
        d.action = ResponseAction::Silence;
        d.tier = ModelTier::Fast;
        d.reason = "agent speaking - do not talk over";
        return d;
    }

    // ---- 规则 2：用户可能还在说（partial 很长且未终结）----
    if (!in.partial_text.empty() && in.partial_text.size() > text.size() + 8) {
        d.action = ResponseAction::Silence;
        d.tier = ModelTier::Fast;
        d.reason = "user still speaking (partial ahead of final)";
        return d;
    }

    // ---- 规则 3：强制静默窗口（TurnDetector 刚判定完成的那一小段）----
    if (silence_window_ms_.load() > 0) {
        d.action = ResponseAction::Silence;
        d.tier = ModelTier::Fast;
        d.reason = "inside forced silence window after turn";
        return d;
    }

    // ---- 规则 4：空输入 → 静默 ----
    if (text.empty()) {
        d.action = ResponseAction::Silence;
        d.tier = ModelTier::Fast;
        d.reason = "empty input";
        return d;
    }

    // ---- 规则 5：trivial 直接快速答，不进大模型 ----
    if (looks_trivial(text)) {
        d.action = ResponseAction::QuickReply;
        d.depth = ResponseDepth::Low;
        d.tier = ModelTier::Fast;
        d.reason = "trivial query - answer without LLM";
        return d;
    }

    // ---- 规则 5.5：情感倾诉 / 闲聊 -> 不查任何东西，就聊天 ----
    // 这条必须在 Search 判定之前。"我最近真的有好多事情"里的"最近"是
    // 时间副词而非查询限定词，误判成 Search 会去搜"最近"然后端一堆无关结果。
    if (looks_like_emotional_smalltalk(text)) {
        d.action = ResponseAction::Answer;
        d.depth = ResponseDepth::Low;
        d.tier = ModelTier::Normal;
        d.needs_search = false;
        d.needs_memory = false;
        d.needs_agent = false;
        d.reason = "emotional smalltalk - just chat, no tools";
        return d;
    }

    // ---- 规则 6：需要实时事实 → Search ----
    if (looks_like_freshness_query(text)) {
        d.needs_search = true;
        d.allow_background = true;
        d.action = ResponseAction::Search;
        d.tier = ModelTier::Search;
        d.latency_budget_ms = cfg_.search_latency_budget_ms;
        d.depth = ResponseDepth::Medium;
        d.reason = "freshness query - search required";
        return d;
    }

    // ---- 规则 7：需要操作本地 → Agent ----
    if (looks_like_local_action(text)) {
        d.needs_agent = true;
        d.allow_background = true;
        d.action = ResponseAction::Agent;
        d.tier = ModelTier::Agent;
        d.latency_budget_ms = cfg_.search_latency_budget_ms;
        d.depth = ResponseDepth::Medium;
        d.reason = "local action requested";
        return d;
    }

    // ---- 规则 8：需要个人上下文 → Memory ----
    if (looks_like_personal_context(text)) {
        d.needs_memory = true;
        d.action = ResponseAction::Answer;
        d.tier = ModelTier::Normal;
        d.depth = ResponseDepth::Medium;
        d.reason = "personal context needed - recall memory";
        return d;
    }

    // ---- 规则 9：复杂推理 → 升级 ----
    if (looks_like_deep_reasoning(text)) {
        d.action = ResponseAction::DeepReasoning;
        d.tier = ModelTier::Deep;
        d.depth = ResponseDepth::High;
        d.latency_budget_ms = cfg_.search_latency_budget_ms;
        d.reason = "complex reasoning - escalate to strong model";
        return d;
    }

    // ---- 规则 10：默认常规回答 ----
    d.action = ResponseAction::Answer;
    d.tier = ModelTier::Normal;
    d.depth = ResponseDepth::Medium;
    d.reason = "default conversational answer";
    return d;
}

// ========== ProgressUtterancePolicy ==========

const std::vector<std::string>& ProgressUtterancePolicy::ack_pool(const std::string& task_type) {
    static const std::map<std::string, std::vector<std::string>> pools = {
        {"search", {
            "我看一下。", "我查一下。", "我确认一下最新的信息。",
            "我找一下相关资料。", "我看看现在的情况。",
        }},
        {"memory", {
            "我翻一下之前的记录。", "我看看你之前提过什么。",
            "我找一下相关的内容。",
        }},
        {"agent", {
            "我来处理一下。", "我动手看一下。", "我操作一下。",
        }},
        {"llm", {
            "我想一下。", "我理一下思路。", "我组织一下说法。",
        }},
    };
    static const std::vector<std::string> empty;
    auto it = pools.find(task_type);
    return it == pools.end() ? empty : it->second;
}

ProgressUtterance ProgressUtterancePolicy::decide(
    const ProgressUtteranceInput& in) const {
    ProgressUtterance u;

    // 话痨闸门：同一任务里已经说过 2 句就不再说了
    if (in.turns_spoken_in_task >= 2) {
        u.kind = ProgressKind::Silent;
        return u;
    }
    // 用户已经在催了 → 立刻闭嘴干活
    if (in.user_seems_impatient) {
        u.kind = ProgressKind::Silent;
        return u;
    }
    // 后台很快能出结果 → 不需要任何 filler
    if (in.expected_latency_ms > 0 && in.expected_latency_ms < 700) {
        u.kind = ProgressKind::Silent;
        return u;
    }
    // 一句话就能说完的活 → 直接答，不用先"我看一下"
    if (in.expected_latency_ms > 0 && in.expected_latency_ms < 400) {
        u.kind = ProgressKind::Silent;
        return u;
    }

    const auto& pool = ack_pool(in.task_type);
    if (pool.empty()) {
        u.kind = ProgressKind::Silent;
        return u;
    }

    // 按 task_type + 已说次数 轮换，避免每次都是同一句
    size_t idx = 0;
    for (char c : in.task_type) idx += static_cast<size_t>(c);
    idx += static_cast<size_t>(in.turns_spoken_in_task);
    u.text = pool[idx % pool.size()];

    // 不复读用户刚说过的话
    if (!in.recent_speech.empty() && u.text == in.recent_speech) {
        u.text = pool[(idx + 1) % pool.size()];
    }

    u.kind = (in.turns_spoken_in_task == 0) ? ProgressKind::Acknowledge
                                            : ProgressKind::Progress;
    return u;
}

}  // namespace voice_agent

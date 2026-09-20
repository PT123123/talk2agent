// src/memory/memory_extractor.cpp
#include "memory_extractor.hpp"
#include "util/log.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace voice_agent {

namespace {

// 简单中英文前缀规则启发式
struct Pattern {
    const char* prefix;     // 匹配话术开头
    const char* type;       // profile / preference / episodic / procedural
    const char* subject;    // "user" 或 "agent"
};

// 按顺序匹配：命中即抽取 content 为整句
const std::vector<Pattern>& patterns() {
    static const std::vector<Pattern> k{
        // 身份 / 属性
        Pattern{"我叫", "profile", "user"},
        Pattern{"我是", "profile", "user"},
        Pattern{"我在", "profile", "user"},
        Pattern{"我住在", "profile", "user"},
        Pattern{"我的名字", "profile", "user"},
        Pattern{"My name is", "profile", "user"},
        Pattern{"I live in", "profile", "user"},
        Pattern{"I'm from", "profile", "user"},
        // 偏好
        Pattern{"我喜", "preference", "user"},
        Pattern{"我喜欢", "preference", "user"},
        Pattern{"我讨厌", "preference", "user"},
        Pattern{"我不喜欢", "preference", "user"},
        Pattern{"我喜欢喝", "preference", "user"},
        Pattern{"I like", "preference", "user"},
        Pattern{"I prefer", "preference", "user"},
        Pattern{"I love", "preference", "user"},
        // 事件 / 目标
        Pattern{"我计划", "episodic", "user"},
        Pattern{"我打算", "episodic", "user"},
        Pattern{"我明天", "episodic", "user"},
        Pattern{"下周", "episodic", "user"},
        Pattern{"I plan", "episodic", "user"},
        Pattern{"I will", "episodic", "user"},
        // 过程性（agent 相关动作偏好）
        Pattern{"请记住", "procedural", "agent"},
        Pattern{"以后叫我", "procedural", "agent"},
        Pattern{"以后都", "procedural", "agent"},
    };
    return k;
}

bool starts_with_ci(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(text[i]);
        unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::isalpha(a) && std::isalpha(b)) {
            if (std::tolower(a) != std::tolower(b)) return false;
        } else if (a != b) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<MemoryExtractResult> MemoryExtractor::extract(const std::string& user_text) const {
    std::vector<MemoryExtractResult> out;
    if (user_text.empty()) return out;
    std::string t = user_text;
    // 去首尾空白
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t' || t.front() == '\r' || t.front() == '\n'))
        t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n'))
        t.pop_back();
    // 去掉句末标点（ASCII 与 CJK 全角）
    static const std::string cjk_punct[] = {
        "\xE3\x80\x82",  // 。
        "\xEF\xBC\x81",  // ！
        "\xEF\xBC\x9F",  // ？
    };
    bool removed = false;
    do {
        removed = false;
        if (t.empty()) break;
        char c = t.back();
        if (c == '?' || c == '!' || c == '.' || c == '\'' || c == '"' || c == '`') {
            t.pop_back();
            removed = true;
            continue;
        }
        for (auto& fw : cjk_punct) {
            if (t.size() >= fw.size() &&
                t.compare(t.size() - fw.size(), fw.size(), fw) == 0) {
                t.resize(t.size() - fw.size());
                removed = true;
                break;
            }
        }
    } while (removed);

    for (auto& p : patterns()) {
        if (starts_with_ci(t, p.prefix)) {
            MemoryExtractResult r;
            r.worth_saving = true;
            r.type = p.type;
            r.subject = p.subject;
            r.content = t;
            r.confidence = 0.8;
            r.ttl_days = (std::string(p.type) == "profile") ? 3650 : 365;
            out.push_back(r);
            break;  // 一句话只抽一条，避免重复
        }
    }
    LOG_DEBUG("MemoryExtractor: {} -> {} candidate(s)", user_text, out.size());
    return out;
}

}  // namespace voice_agent
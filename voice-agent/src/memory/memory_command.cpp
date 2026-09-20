// src/memory/memory_command.cpp
#include "memory_command.hpp"
#include "util/log.hpp"
#include <algorithm>
#include <sstream>
#include <cctype>

namespace voice_agent {

namespace {
std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}
std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}
}  // namespace

bool is_memory_command(const std::string& text) {
    auto t = trim(text);
    return lower(t).rfind("memory ", 0) == 0 || lower(t).rfind("/memory", 0) == 0;
}

std::string run_memory_command(MemoryStore& store, const std::string& cmdline) {
    auto t = trim(cmdline);
    // 去掉开头的 "memory" / "/memory"
    if (lower(t).rfind("/memory", 0) == 0) {
        t = t.substr(7);
    } else if (lower(t).rfind("memory ", 0) == 0) {
        t = t.substr(7);
    }
    t = trim(t);

    // 取子命令与余下参数
    auto sp = t.find_first_of(" \t");
    std::string verb = (sp == std::string::npos) ? t : t.substr(0, sp);
    std::string rest = (sp == std::string::npos) ? "" : trim(t.substr(sp + 1));
    verb = lower(verb);

    if (verb == "save" || verb == "记" || verb == "记住") {
        if (rest.empty()) return "你想记住什么呢？告诉我内容即可。";
        // 尝试解析 "主题@内容" 或整句作为内容，subject 默认 user
        std::string subject = "user";
        std::string content = rest;
        auto at = rest.find('@');
        if (at != std::string::npos && at > 0) {
            subject = trim(rest.substr(0, at));
            content = trim(rest.substr(at + 1));
        }
        int64_t id = store.save(subject, content);
        if (id < 0) return "保存失败。";
        return "已记住：" + content;
    }

    if (verb == "list" || verb == "列表") {
        int n = 10;
        if (!rest.empty()) {
            try { n = std::max(1, std::stoi(rest)); } catch (...) { n = 10; }
        }
        auto items = store.list(n);
        if (items.empty()) return "当前没有保存的记忆。";
        std::ostringstream o;
        o << "共" << items.size() << "条记忆。";
        for (auto& m : items) o << " " << m.id << "号：" << m.content << "。";
        return o.str();
    }

    if (verb == "count" || verb == "数量") {
        return "已保存" + std::to_string(store.count()) + "条记忆。";
    }

    if (verb == "clear" || verb == "清空") {
        store.clear();
        return "已清空所有记忆。";
    }

    if (verb == "forget" || verb == "删除" || verb == "忘掉") {
        if (rest.empty()) return "请告诉我记忆的编号或关键词。";
        // 尝试按 id 删除
        char* end = nullptr;
        long id = std::strtol(rest.c_str(), &end, 10);
        bool numeric = end && *end == '\0' && rest[0] != '\0';
        if (numeric && store.remove(id)) return "已删除记忆" + std::to_string(id) + "号。";
        // 按关键词删除：查最相关的那条
        auto hits = store.query(rest, 1);
        if (hits.empty()) return "没有找到相关记忆。";
        store.remove(hits[0].id);
        return "已删除与“" + rest + "”相关的记忆：" + hits[0].content;
    }

    // 兜底：把整句当查询
    if (!verb.empty() && !rest.empty()) {
        auto hits = store.query(verb + " " + rest, 3);
        if (hits.empty()) return "没有找到相关记忆。";
        std::ostringstream o;
        for (auto& m : hits) o << (m.id) << "号：" << m.content << "。";
        return o.str();
    }
    return "记忆命令：memory save/list/forget/clear/count。";
}

}  // namespace voice_agent
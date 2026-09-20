// src/agent/tools.cpp
#include "tools.hpp"
#include "memory/memory_store.hpp"
#include "util/log.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <sstream>

namespace voice_agent {

using json = nlohmann::json;

// ========== 内置工具实现 ==========

namespace {

std::string current_time_string() {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string format_search_results(const std::vector<SearchHit>& hits, int topk) {
    json arr = json::array();
    int n = 0;
    for (auto& h : hits) {
        if (n >= topk) break;
        arr.push_back({
            {"title", h.title},
            {"url", h.url},
            {"snippet", h.snippet},
        });
        ++n;
    }
    if (arr.empty()) return "no results";
    return arr.dump();
}

}  // namespace

// ========== 注册内置工具 ==========

void register_builtin_tools(ToolRegistry& registry, const ToolKit& kit) {
    // ---- get_time ----
    registry.register_tool(
        "get_time", "返回当前日期时间（本地时区，格式 YYYY-MM-DD HH:MM:SS）。",
        json{{"type", "object"}, {"properties", json::object()}, {"required", json::array()}},
        [](const json&, std::shared_ptr<CancelToken>) -> ToolResult {
            ToolResult r;
            r.content = current_time_string();
            return r;
        });

    // ---- web_search ----
    json search_schema = {
        {"type", "object"},
        {"properties",
         {{"query", {{"type", "string"}, {"description", "搜索关键词，简洁具体"}}},
          {"topk", {{"type", "integer"}, {"description", "返回条数，默认 6"}}},
          {"time_range",
           {{"type", "string"}, {"description", "可选：day/week/month/year"}}}}},
        {"required", json::array({"query"})}};
    registry.register_tool(
        "web_search",
        "进行联网搜索，返回带标题、链接与摘要的结果。用于查询最新/事实类信息。",
        search_schema,
        [kit](const json& args, std::shared_ptr<CancelToken> cancel) -> ToolResult {
            ToolResult r;
            if (!kit.search) {
                r.is_error = true;
                r.content = "web_search unavailable: search not configured";
                return r;
            }
            SearchQuery q;
            q.q = args.value("query", std::string(""));
            q.topk = args.value("topk", 6);
            if (args.contains("time_range") && args["time_range"].is_string())
                q.time_range = args["time_range"].get<std::string>();
            if (q.q.empty()) {
                r.is_error = true;
                r.content = "web_search requires non-empty 'query'";
                return r;
            }
            auto hits = kit.search->query(q, SearchPolicy::LocalFirst, cancel);
            r.content = format_search_results(hits, q.topk);
            return r;
        });

    // ---- memory_save ----
    json save_schema = {
        {"type", "object"},
        {"properties",
         {{"subject", {{"type", "string"}, {"description", "主体，如 user/agent"}}},
          {"content", {{"type", "string"}, {"description", "要长期记住的内容"}}}}},
        {"required", json::array({"content"})}};
    registry.register_tool(
        "memory_save", "把一条用户事实或偏好存入长期记忆，供未来对话引用。",
        save_schema,
        [kit](const json& args, std::shared_ptr<CancelToken>) -> ToolResult {
            ToolResult r;
            if (!kit.memory) {
                r.is_error = true;
                r.content = "memory unavailable: not configured";
                return r;
            }
            std::string subject = args.value("subject", std::string("user"));
            std::string content = args.value("content", std::string(""));
            if (content.empty()) {
                r.is_error = true;
                r.content = "memory_save requires non-empty 'content'";
                return r;
            }
            int64_t id = kit.memory->save(subject, content);
            if (id < 0) {
                r.is_error = true;
                r.content = "memory_save failed to persist";
                return r;
            }
            r.content = "saved (id=" + std::to_string(id) + ")";
            return r;
        });

    // ---- memory_query ----
    json query_schema = {
        {"type", "object"},
        {"properties",
         {{"q", {{"type", "string"}, {"description", "查询关键词"}}}}},
        {"required", json::array({"q"})}};
    registry.register_tool(
        "memory_query", "按关键词检索长期记忆，返回相关条目。",
        query_schema,
        [kit](const json& args, std::shared_ptr<CancelToken>) -> ToolResult {
            ToolResult r;
            if (!kit.memory) {
                r.is_error = true;
                r.content = "memory unavailable: not configured";
                return r;
            }
            std::string q = args.value("q", std::string(""));
            auto items = kit.memory->query(q, 4);
            if (items.empty()) {
                r.content = "no memory found";
                return r;
            }
            json arr = json::array();
            for (auto& m : items) {
                arr.push_back({{"subject", m.subject}, {"content", m.content}});
            }
            r.content = arr.dump();
            return r;
        });
}

}  // namespace voice_agent
// src/agent/tools.hpp
#pragma once
#include "agent/tool_registry.hpp"
#include "search/isearch.hpp"
#include <memory>
#include <string>
#include <vector>

namespace voice_agent {

class MemoryStore;  // 定义见 memory/memory_store.hpp

// ========== 工具依赖集合 ==========
struct ToolKit {
    std::shared_ptr<SearchRouter> search;    // web_search 使用
    std::shared_ptr<MemoryStore> memory;     // memory_* 使用（持久化）
};

// 注册内置工具：get_time / web_search / memory_save / memory_query
void register_builtin_tools(ToolRegistry& registry, const ToolKit& kit);

// ========== 工具意图门控 ==========
// 根据用户本轮文本判断"明确要求了哪些工具"，返回允许使用的工具名集合。
// 未命中任何意图时返回空集合 → AgentLoop 完全不暴露工具，模型无从调用，
// 从根本上杜绝"每轮都调用 web_search / memory_*"的误触发。
std::vector<std::string> detect_tool_intent(const std::string& user_text);

}  // namespace voice_agent
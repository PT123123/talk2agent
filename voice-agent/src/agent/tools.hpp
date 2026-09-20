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

}  // namespace voice_agent
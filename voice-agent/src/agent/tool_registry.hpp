// src/agent/tool_registry.hpp
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/types.hpp"
#include "core/cancel_token.hpp"
#include <nlohmann/json.hpp>

namespace voice_agent {

// ========== 工具处理器 ==========
// receive: 已解析的参数 JSON + 取消令牌
// return : 处理结果
using ToolHandler = std::function<ToolResult(
    const nlohmann::json& args, std::shared_ptr<CancelToken>)>;

// ========== 工具注册中心 ==========
class ToolRegistry {
public:
    ToolRegistry() = default;

    // 注册工具；重名会覆盖并返回 false。
    bool register_tool(std::string name, std::string description,
                       nlohmann::json arg_schema, ToolHandler handler);

    bool unregister(const std::string& name);

    bool has(const std::string& name) const;
    const ToolHandler* handler(const std::string& name) const;

    std::vector<ToolDef> tool_defs() const;   // 供 LLM 使用
    std::vector<std::string> names() const;

private:
    struct Entry {
        ToolHandler handler;
        nlohmann::json schema;
        std::string description;
    };
    std::unordered_map<std::string, Entry> tools_;
};

// ========== 并行执行器 ==========
// 每个工具在一个 worker 线程运行；支持单个超时与整体取消。
class ToolExecutor {
public:
    explicit ToolExecutor(ToolRegistry& registry) : registry_(registry) {}

    // 并行执行多个工具调用。
    // - 每个调用超时 timeout_ms（超时返回 "工具超时" 错误结果，不卡死）
    // - 共享 cancel：任一时刻 cancel 触发则剩余等待的调用直接返回被取消
    // - 返回与 calls 一一对应的结果
    std::vector<ToolResult> execute_parallel(
        const std::vector<ToolCall>& calls,
        std::shared_ptr<CancelToken> cancel,
        int timeout_ms = 5000);

    // 顺序执行单个
    ToolResult execute_one(const ToolCall& call, std::shared_ptr<CancelToken> cancel,
                           int timeout_ms = 5000);

private:
    ToolRegistry& registry_;
};

}  // namespace voice_agent
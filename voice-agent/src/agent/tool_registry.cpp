// src/agent/tool_registry.cpp
#include "tool_registry.hpp"
#include "util/log.hpp"
#include "grammar.hpp"
#include <chrono>
#include <future>
#include <set>

namespace voice_agent {

using json = nlohmann::json;
using namespace std::chrono_literals;

// ========== ToolRegistry ==========

bool ToolRegistry::register_tool(std::string name, std::string description,
                                 json arg_schema, ToolHandler handler) {
    if (name.empty() || !handler) return false;
    Entry e;
    e.description = std::move(description);
    e.schema = std::move(arg_schema);
    e.handler = std::move(handler);
    auto [it, inserted] = tools_.insert_or_assign(std::move(name), std::move(e));
    (void)it;
    LOG_INFO("Tool registered: {} (inserted={})", it->first, inserted);
    return true;
}

bool ToolRegistry::unregister(const std::string& name) {
    return tools_.erase(name) > 0;
}

bool ToolRegistry::has(const std::string& name) const {
    return tools_.find(name) != tools_.end();
}

const ToolHandler* ToolRegistry::handler(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second.handler;
}

std::vector<ToolDef> ToolRegistry::tool_defs() const {
    std::vector<ToolDef> defs;
    for (auto& [name, e] : tools_) {
        ToolDef d;
        d.name = name;
        d.description = e.description;
        d.json_schema = e.schema.is_null() ? "{}" : e.schema.dump();
        defs.push_back(std::move(d));
    }
    return defs;
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> out;
    for (auto& [name, e] : tools_) out.push_back(name);
    return out;
}

// ========== ToolExecutor ==========

ToolResult ToolExecutor::execute_one(const ToolCall& call, std::shared_ptr<CancelToken> cancel,
                                     int timeout_ms) {
    ToolResult res;
    res.call_id = call.id;

    const ToolHandler* h = registry_.handler(call.name);
    if (!h) {
        res.is_error = true;
        res.content = "tool not found: " + call.name;
        LOG_WARN("Unknown tool call: {}", call.name);
        return res;
    }

    // 离线校验参数
    std::string verr = validate_tool_args_json(call.arguments_json, call.name);
    if (!verr.empty()) {
        res.is_error = true;
        res.content = "invalid arguments for '" + call.name + "': " + verr;
        LOG_WARN("{}", res.content);
        return res;
    }

    json args = json::parse(call.arguments_json.empty() ? "{}" : call.arguments_json);
    if (cancel && cancel->cancelled()) {
        res.is_error = true;
        res.content = "cancelled before execution";
        return res;
    }

    auto fut = std::async(std::launch::async, [h, args, cancel]() {
        return (*h)(args, cancel);
    });

    // 轮询等待，期间监测取消
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel && cancel->cancelled()) {
            res.is_error = true;
            res.content = "cancelled";
            return res;
        }
        if (fut.wait_for(1ms) == std::future_status::ready) {
            try {
                return fut.get();
            } catch (const std::exception& e) {
                res.is_error = true;
                res.content = std::string("tool threw: ") + e.what();
                return res;
            }
        }
    }

    res.is_error = true;
    res.content = "tool timeout (> " + std::to_string(timeout_ms) + "ms)";
    LOG_WARN("Tool {} timed out", call.name);
    return res;
}

std::vector<ToolResult> ToolExecutor::execute_parallel(
    const std::vector<ToolCall>& calls, std::shared_ptr<CancelToken> cancel, int timeout_ms) {
    std::vector<ToolResult> results;
    results.reserve(calls.size());

    std::vector<std::future<ToolResult>> futs;
    futs.reserve(calls.size());
    for (auto& call : calls) {
        futs.push_back(std::async(std::launch::async, [this, &call, timeout_ms]() -> ToolResult {
            return execute_one(call, nullptr, timeout_ms);
        }));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms + 50);
    for (auto& f : futs) {
        if (cancel && cancel->cancelled()) {
            ToolResult r;
            r.is_error = true;
            r.content = "cancelled";
            results.push_back(std::move(r));
            continue;
        }
        auto status = f.wait_until(deadline);
        if (status == std::future_status::ready) {
            try {
                results.push_back(f.get());
            } catch (...) {
                ToolResult r;
                r.is_error = true;
                r.content = "tool exception";
                results.push_back(std::move(r));
            }
        } else {
            ToolResult r;
            r.is_error = true;
            r.content = "tool timeout";
            results.push_back(std::move(r));
        }
    }
    return results;
}

}  // namespace voice_agent
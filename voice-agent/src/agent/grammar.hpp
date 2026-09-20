// src/agent/grammar.hpp
#pragma once
#include <string>
#include <vector>
#include "core/types.hpp"
#include <nlohmann/json.hpp>

namespace voice_agent {

// ========== 工具调用可夹带在正文中，也可作为独立输出 ==========

// 工具调用在助手文本中的包裹标记。
// 模型输出形如：
//   让我查一下。
//   <tool_call>{"name":"web_search","arguments":{...}}</tool_call>
// 支持多个工具并行（每行一个 <tool_call>）。
constexpr const char* kToolCallOpen  = "<tool_call>";
constexpr const char* kToolCallClose = "</tool_call>";

// 把 JSON Schema（tools 的 argument schema）+ 工具名转换为 GBNF 约束文法。
// 生成的 grammar 强制模型输出：
//   {"tool_calls":[{...}]}  —— 供需要严格只输出工具调用时使用。
// 若 use_any_text 为 true，则生成一个允许"先任意文本后工具调用"的宽松文法（占位）。
std::string tool_calls_grammar(const std::vector<ToolDef>& tools);

// 仅转换单个 argument JSON Schema 为 GBNF 类型规则（对象/字符串/整数/数字/布尔/枚举/数组）。
std::string json_schema_to_gbnf(const nlohmann::json& schema, std::string rule_name,
                                std::vector<std::string>* extra_rules);

// 从助手文本中解析出工具调用（按 kToolCallOpen/Close 包裹块提取）。
// 解析失败的块会跳过。解析结果按其出现顺序返回。
std::vector<ToolCall> parse_tool_calls(const std::string& assistant_text);

// 用 llama.cpp 的 GBNF 字符串为给定 schema 校验文本（离线近似：仅 JSON 合法性 + 必填字段）。
// 返回非空字符串表示校验通过，返回空表示含非法内容。
std::string validate_tool_args_json(const std::string& args_json, const std::string& name);

}  // namespace voice_agent
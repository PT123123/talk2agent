// src/agent/grammar.cpp
#include "grammar.hpp"
#include "util/log.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <map>

namespace voice_agent {

using json = nlohmann::json;

namespace {

// 标准 JSON GBNF（llama.cpp common 变体），root 名可指定
std::string any_json_block(const std::string& root_name) {
    std::ostringstream o;
    o << root_name << " ::= object\n"
      << "object ::= \"{\" ws ( string \":\" ws value ( \",\" ws string \":\" ws value )* )? \"}\" ws\n"
      << "array  ::= \"[\" ws ( value ( \",\" ws value )* )? \"]\" ws\n"
      << "value  ::= object | array | string | number | boolean | null\n"
      << "string ::= \"\\\"\" ( [^\"\\\\\\x7F\\x00-\\x1F] | \"\\\\\" ( [\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] ) )* \"\\\"\" ws\n"
      << "number ::= (\"-\"? ([0-9] | [1-9] [0-9]*)) (\".\" [0-9]+)? ([eE] [-+]? [0-9]+)? ws\n"
      << "boolean ::= (\"true\" | \"false\") ws\n"
      << "null    ::= \"null\" ws\n"
      << "ws      ::= [ \\t\\n]*\n";
    return o.str();
}

// 为顶层的 tool-call wrapper 生成 name 枚举（只允许已注册工具名）
std::string name_enum(const std::vector<ToolDef>& tools) {
    std::ostringstream o;
    o << "name-expr ::= ";
    for (size_t i = 0; i < tools.size(); ++i) {
        if (i) o << " | ";
        o << "\"" << tools[i].name << "\"";
    }
    o << "\n";
    return o.str();
}

std::string schema_type_name(const std::string& base) {
    return "arg-" + base;
}

}  // namespace

// ========== json_schema_to_gbnf：把参数 schema 转为类型化 object 规则 ==========

std::string json_schema_to_gbnf(const json& schema, std::string rule_name,
                                std::vector<std::string>* extra_rules) {
    std::vector<std::string> rules;
    std::string type = schema.value("type", "object");

    if (type == "object") {
        std::ostringstream o;
        o << rule_name << " ::= \"{\" ws \n";
        // 必填与可选属性
        if (schema.contains("properties") && schema["properties"].is_object()) {
            const auto& props = schema["properties"];
            std::vector<std::string> required;
            if (schema.contains("required") && schema["required"].is_array()) {
                for (auto& r : schema["required"]) required.push_back(r.get<std::string>());
            }
            std::vector<std::string> names;
            for (auto& [k, v] : props.items()) {
                auto it = std::find(required.begin(), required.end(), k);
                if (it == required.end()) continue;  // 只约束必填项，可选不强制
                names.push_back(k);
                std::string sub = schema_type_name(k);
                rules.push_back(json_schema_to_gbnf(v, sub, extra_rules));
            }
            for (size_t i = 0; i < names.size(); ++i) {
                if (i) o << " \",\" ws\n";
                o << " \"" << names[i] << "\" ws \":\" ws " << schema_type_name(names[i]) << "\n";
            }
        }
        o << "\"}\" ws\n";
        std::string text = o.str();
        if (extra_rules) {
            extra_rules->push_back(text);
            extra_rules->insert(extra_rules->end(), rules.begin(), rules.end());
            return "";
        }
        std::ostringstream full;
        full << text;
        for (auto& r : rules) full << r;
        full << any_json_block("value");
        return full.str();
    }

    // 标量类型直接映射到共享规则名
    std::string mapped;
    if (type == "string")      mapped = "string";
    else if (type == "number") mapped = "number";
    else if (type == "integer") mapped = "integer";
    else if (type == "boolean") mapped = "boolean";
    else if (type == "null")    mapped = "null";
    else if (type == "array")   mapped = "array";
    else mapped = "value";

    std::string text = rule_name + " ::= " + mapped + "\n";
    if (extra_rules) {
        extra_rules->push_back(text);
        return "";
    }
    return text + any_json_block("value");
}

// ========== tool_calls_grammar：wrapper（枚举 name + 任意 json 参数）==========

std::string tool_calls_grammar(const std::vector<ToolDef>& tools) {
    std::ostringstream o;
    o << "root ::= tool-calls\n"
      << "tool-calls ::= \"{\" ws \"\\\"tool_calls\\\"\" ws \":\" ws \"[\" ws ( tool-call ( \",\" ws tool-call )* )? \"]\" ws \"}\" ws\n"
      << "tool-call  ::= \"{\" ws \"\\\"name\\\"\" ws \":\" ws "
      << "\"\\\"\" ws name-expr \"\\\"\" ws \",\" ws \"\\\"arguments\\\"\" ws \":\" ws value ws \"}\" ws\n";
    o << name_enum(tools);
    o << any_json_block("value");
    return o.str();
}

// ========== parse_tool_calls：从助手文本提取工具调用 ==========

std::vector<ToolCall> parse_tool_calls(const std::string& assistant_text) {
    std::vector<ToolCall> calls;
    size_t pos = 0;
    while (true) {
        size_t open = assistant_text.find(kToolCallOpen, pos);
        if (open == std::string::npos) break;
        size_t body_start = open + std::string(kToolCallOpen).size();
        size_t close = assistant_text.find(kToolCallClose, body_start);
        if (close == std::string::npos) break;

        std::string json_str = assistant_text.substr(body_start,
                                                   close - body_start);
        // 去掉首尾空白后尝试解析
        auto trim = [](std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                                  s.front() == '\n' || s.front() == '\r'))
                s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                                  s.back() == '\n' || s.back() == '\r'))
                s.pop_back();
            return s;
        };
        json_str = trim(std::move(json_str));
        try {
            auto j = json::parse(json_str);
            ToolCall tc;
            tc.name = j.value("name", "");
            if (tc.name.empty()) {
                LOG_WARN("Tool call block missing 'name'");
                pos = close + 1;
                continue;
            }
            if (j.contains("arguments") && j["arguments"].is_object())
                tc.arguments_json = j["arguments"].dump();
            else if (j.contains("arguments"))
                tc.arguments_json = j["arguments"].dump();
            else
                tc.arguments_json = "{}";
            tc.id = "tc_" + std::to_string(calls.size());
            calls.push_back(std::move(tc));
        } catch (const std::exception& e) {
            LOG_WARN("Dropping malformed tool call block: {}", e.what());
        }
        pos = close + 1;
    }
    return calls;
}

// ========== validate_tool_args_json：离线校验参数 JSON ==========

namespace {
struct FieldRule { std::string key; std::string type; };
std::map<std::string, std::vector<FieldRule>> required_fields() {
    return {
        {"web_search",   {{"query", "string"}}},
        {"memory_save",  {{"content", "string"}}},
        {"memory_query", {{"q", "string"}}},
        {"get_time",     {}},
    };
}
}

std::string validate_tool_args_json(const std::string& args_json, const std::string& name) {
    json j;
    try {
        j = json::parse(args_json);
    } catch (const std::exception& e) {
        return std::string("arguments not valid JSON: ") + e.what();
    }
    if (!j.is_object()) return "arguments must be a JSON object";

    auto rules = required_fields();   // 持有副本，保证 iterator 生命周期
    auto it = rules.find(name);
    if (it == rules.end()) return "";  // 未知工具不额外约束

    for (auto& rule : it->second) {
        if (j.contains(rule.key)) {
            const auto& v = j[rule.key];
            bool ok = (rule.type == "string" && v.is_string())   ||
                      (rule.type == "number" && (v.is_number())) ||
                      (rule.type == "integer" && v.is_number())  ||
                      (rule.type == "boolean" && v.is_boolean());
            if (!ok) return "field '" + rule.key + "' must be " + rule.type;
        } else {
            return "missing required field '" + rule.key + "'";
        }
    }
    return "";
}

}  // namespace voice_agent